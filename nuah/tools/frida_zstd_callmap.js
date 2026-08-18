'use strict';

/*
 * Read-only, late-attach call mapper for trace-enabled Zstd embedded in the
 * current Roblox image. Nuah provides the two weak ZSTD_trace_* imports, so
 * their begin/end callbacks give a precise Stalker window around a decode.
 *
 * This intentionally makes no replacement and does not alter arguments or
 * return values. Its output contains only libroblox-relative call targets.
 */

const minimumDurationMs = 5;
const sessions = new Map();

function robloxBase() {
  for (const range of Process.enumerateRanges('---')) {
    if (range.file !== undefined &&
        range.file.path.endsWith('/libroblox.so') && range.file.offset === 0) {
      return range.base;
    }
  }
  return null;
}

function globalExport(name) {
  if (typeof Module.findGlobalExportByName === 'function')
    return Module.findGlobalExportByName(name);
  return Module.getGlobalExportByName(name);
}

function attach() {
  const base = robloxBase();
  const begin = globalExport('ZSTD_trace_decompress_begin');
  const end = globalExport('ZSTD_trace_decompress_end');
  if (base === null || begin === null || end === null) return false;

  Interceptor.attach(begin, {
    onEnter() {
      const tid = this.threadId;
      if (sessions.has(tid)) return;
      const session = { start: Date.now(), calls: new Map() };
      sessions.set(tid, session);
      Stalker.follow(tid, {
        events: { call: true },
        onCallSummary(summary) {
          for (const [target, count] of Object.entries(summary)) {
            session.calls.set(target, (session.calls.get(target) || 0) + count);
          }
        },
      });
    },
  });

  Interceptor.attach(end, {
    onEnter() {
      const tid = this.threadId;
      const session = sessions.get(tid);
      if (session === undefined) return;
      Stalker.unfollow(tid);
      Stalker.flush();
      sessions.delete(tid);
      const elapsed = Date.now() - session.start;
      setTimeout(function () {
        if (elapsed < minimumDurationMs) return;
        const targets = Array.from(session.calls.entries())
            .map(([address, count]) => ({
              address, count,
            }))
            .sort((left, right) => right.count - left.count)
            .slice(0, 20);
        send({ kind: 'zstd_callmap', elapsed_ms: elapsed, targets });
        Stalker.garbageCollect();
      }, 0);
    },
  });
  send({kind: 'ready', begin: begin.toString(), end: end.toString()});
  return true;
}

const timer = setInterval(function () {
  if (attach()) clearInterval(timer);
}, 50);

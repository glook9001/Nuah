'use strict';

/* Read-only runtime probe for choosing the safe RbxStorage interception
 * boundary. It reports only SQLite API call counts—never SQL text, paths,
 * cache IDs, or session data. */
const module = Process.enumerateModules().find(m => m.name.includes('sqlite3'));
if (module === undefined) {
  send({ error: 'sqlite module not loaded' });
} else {
  const counts = {};
  const symbols = [
    'sqlite3_column_blob', 'sqlite3_column_bytes', 'sqlite3_blob_open',
    'sqlite3_blob_read', 'sqlite3_step', 'sqlite3_prepare_v2'
  ];
  for (const symbol of symbols) {
    counts[symbol] = 0;
    const address = module.findExportByName(symbol);
    if (address !== null) {
      Interceptor.attach(address, { onEnter() { counts[symbol]++; } });
    }
  }
  send({ sqlite_module: module.name, hooks: Object.keys(counts) });
  const report = () => {
    send({ counts });
    setTimeout(report, 1000);
  };
  setTimeout(report, 1000);
}

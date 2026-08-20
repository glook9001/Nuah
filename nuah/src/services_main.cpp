#include "nuah/protocol.hpp"
#include "nuah/sober_cache.hpp"

#include <adwaita.h>
#include <glib-unix.h>
#include <webkit/webkit.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <cstdint>

namespace {
struct ServiceState {
  GApplication* app = nullptr;
  GtkWindow* window = nullptr;
  GtkStack* stack = nullptr;
  GtkLabel* status = nullptr;
  GtkButton* play_button = nullptr;
  GtkButton* session_button = nullptr;
  GtkSearchEntry* search_entry = nullptr;
  WebKitWebView* web_view = nullptr;
  WebKitUserContentManager* bridge = nullptr;
  int server_fd = -1;
  std::filesystem::path data_directory;
  nuah::SoberCacheStatus package;
};

struct ServiceLaunch {
  int server_fd = -1;
  bool detached = false;
};

void set_status(ServiceState* state, const char* text);

void free_soup_cookie(gpointer value) {
  soup_cookie_free(static_cast<SoupCookie*>(value));
}

std::filesystem::path data_directory() {
  if (const char* configured = std::getenv("NUAH_DATA_DIR");
      configured && *configured) {
    return std::filesystem::absolute(configured);
  }
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "nuah";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local/share/nuah";
  }
  return std::filesystem::temp_directory_path() / "nuah";
}

void set_status(ServiceState* state, const char* text) {
  // WebKit completion callbacks may arrive during teardown.  Do not touch a
  // label that GTK has already disposed while the Services process exits.
  if (state && GTK_IS_LABEL(state->status)) {
    gtk_label_set_text(state->status, text ? text : "");
  }
}

bool send_to_supervisor(ServiceState* state, const std::string& json) {
  if (state->server_fd < 0) return false;
  std::string error;
  if (!nuah::send_sober_webkit_json(state->server_fd, json, error)) {
    state->server_fd = -1;
    set_status(state, error.c_str());
    return false;
  }
  return true;
}

void javascript_finished(GObject* source, GAsyncResult* result, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  if (error) {
    set_status(state, error->message);
    send_to_supervisor(state, R"({"type":"web.evaluated","ok":false})");
    g_error_free(error);
    return;
  }
  if (value) g_object_unref(value);
  send_to_supervisor(state, R"({"type":"web.evaluated","ok":true})");
}

void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (event == WEBKIT_LOAD_STARTED) {
    set_status(state, "Connecting securely to Roblox…");
  } else if (event == WEBKIT_LOAD_FINISHED) {
    const char* title = webkit_web_view_get_title(view);
    set_status(state, title && *title ? title : "Roblox web page loaded");
    // Roblox sets the authenticated cookie asynchronously while its login
    // page navigates.  Stream it to the supervisor and enable Logout after
    // each completed navigation instead of guessing from the page title.
    auto* cookie_manager = webkit_network_session_get_cookie_manager(
        webkit_web_view_get_network_session(view));
    webkit_cookie_manager_get_cookies(
        cookie_manager, "https://www.roblox.com/", nullptr,
        [](GObject* source, GAsyncResult* result, gpointer user_data) {
          auto* service = static_cast<ServiceState*>(user_data);
          GError* error = nullptr;
          GList* cookies = webkit_cookie_manager_get_cookies_finish(
              WEBKIT_COOKIE_MANAGER(source), result, &error);
          if (error) {
            g_error_free(error);
            return;
          }
          SoupCookie* session_cookie = nullptr;
          for (GList* item = cookies; item; item = item->next) {
            auto* cookie = static_cast<SoupCookie*>(item->data);
            if (g_strcmp0(soup_cookie_get_name(cookie),
                          ".ROBLOSECURITY") == 0) {
              session_cookie = cookie;
              break;
            }
          }
          const bool signed_in = (session_cookie != nullptr);
          if (session_cookie) {
            const char* value = soup_cookie_get_value(session_cookie);
            if (value && *value) {
              gchar* encoded = g_base64_encode(
                  reinterpret_cast<const guchar*>(value), std::strlen(value));
              const std::string event =
                  std::string(R"({"type":"web.session_cookie","value_b64":")") +
                  encoded + R"("})";
              g_free(encoded);
              send_to_supervisor(service, event);
            }
          }
          g_list_free_full(cookies, free_soup_cookie);
          if (service->session_button) {
            gtk_widget_set_sensitive(GTK_WIDGET(service->session_button),
                                     signed_in);
          }
          if (signed_in) {
            set_status(service, "Signed in to Roblox");
          }
        },
        state);
  }
}

// Roblox pages can invoke the native client either through the Hybrid bridge
// or by navigating to the registered desktop URI.  Sober owns both routes;
// Nuah must not let the latter fall through to WebKit's generic “download the
// client” handling.
gboolean on_decide_policy(WebKitWebView*, WebKitPolicyDecision* decision,
                          WebKitPolicyDecisionType type, gpointer data) {
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
  auto* navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
  auto* action = webkit_navigation_policy_decision_get_navigation_action(
      navigation);
  auto* request = action ? webkit_navigation_action_get_request(action) : nullptr;
  const char* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
  if (!uri || (!g_str_has_prefix(uri, "roblox://") &&
               !g_str_has_prefix(uri, "roblox-player://"))) {
    return FALSE;
  }
  auto* state = static_cast<ServiceState*>(data);
  // Roblox URIs are percent-encoded by the page.  Reject an unrepresentable
  // value rather than building malformed JSON for the private socket.
  if (std::strchr(uri, '\"') || std::strchr(uri, '\\')) {
    set_status(state, "Roblox supplied an invalid launch URI");
  } else if (send_to_supervisor(
                 state, std::string(R"({"type":"runtime.uri","uri":")") +
                            uri + R"("})")) {
    set_status(state, "Opening Roblox game…");
  } else {
    set_status(state, "No Nuah runtime supervisor is connected");
  }
  webkit_policy_decision_ignore(decision);
  return TRUE;
}

void on_play_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (send_to_supervisor(state, R"({"type":"runtime.open"})")) {
    set_status(state, "Requested Roblox from the runtime supervisor…");
    return;
  }
  set_status(state, "Services needs a connected Nuah runtime supervisor");
}

void session_cookies_finished(GObject* source, GAsyncResult* result,
                              gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  GError* error = nullptr;
  GList* cookies = webkit_cookie_manager_get_cookies_finish(
      WEBKIT_COOKIE_MANAGER(source), result, &error);
  if (error) {
    set_status(state, error->message);
    g_error_free(error);
    return;
  }
  SoupCookie* session = nullptr;
  for (GList* item = cookies; item; item = item->next) {
    auto* cookie = static_cast<SoupCookie*>(item->data);
    if (g_strcmp0(soup_cookie_get_name(cookie), ".ROBLOSECURITY") == 0) {
      session = cookie;
      break;
    }
  }
  if (!session) {
    g_list_free_full(cookies, free_soup_cookie);
    set_status(state, "Sign in to Roblox in this page first");
    return;
  }
  const char* value = soup_cookie_get_value(session);
  gchar* encoded = g_base64_encode(reinterpret_cast<const guchar*>(value),
                                   std::strlen(value));
  const std::string event =
      std::string(R"({"type":"web.session_cookie","value_b64":")") +
      encoded + R"("})";
  g_free(encoded);
  g_list_free_full(cookies, free_soup_cookie);
  if (send_to_supervisor(state, event)) {
    set_status(state, "Sending your Roblox browser session to Android…");
  } else {
    set_status(state, "No Nuah runtime supervisor is connected");
  }
}

void on_logout_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (!state->web_view) return;
  auto* session = webkit_web_view_get_network_session(state->web_view);
  auto* cookie_manager = webkit_network_session_get_cookie_manager(session);
  webkit_cookie_manager_replace_cookies(cookie_manager, nullptr, nullptr,
                                        nullptr, nullptr);
  auto* data_manager = webkit_network_session_get_website_data_manager(session);
  if (data_manager) {
    webkit_website_data_manager_clear(
        data_manager,
        static_cast<WebKitWebsiteDataTypes>(WEBKIT_WEBSITE_DATA_COOKIES |
                                            WEBKIT_WEBSITE_DATA_DOM_CACHE |
                                            WEBKIT_WEBSITE_DATA_LOCAL_STORAGE |
                                            WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES),
        0, nullptr, nullptr, nullptr);
  }
  std::error_code ec;
  std::filesystem::remove(state->data_directory / "cookies", ec);
  send_to_supervisor(state, R"({"type":"web.session_clear"})");
  set_status(state, "Logged out — loading login page…");
  if (state->session_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->session_button), false);
  }
  webkit_web_view_load_uri(state->web_view, "https://www.roblox.com/login");
}

void on_use_browser_session_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (!state->web_view) return;
  auto* cookie_manager = webkit_network_session_get_cookie_manager(
      webkit_web_view_get_network_session(state->web_view));
  webkit_cookie_manager_get_cookies(
      cookie_manager, "https://www.roblox.com/", nullptr,
      session_cookies_finished, state);
}


void on_script_message(
    WebKitUserContentManager*, JSCValue* value, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  // Android posts the four-field query directly through executeRoblox.  The
  // iOS-compatible RobloxWKHybrid channel wraps the same query as `command`.
  // Supporting both gives Nuah one WebKit bridge contract instead of two
  // different login/launch implementations.
  JSCValue* command = nullptr;
  if (jsc_value_is_object(value)) {
    command = jsc_value_object_get_property(value, "command");
  }
  gchar* json = command && jsc_value_is_string(command)
                    ? jsc_value_to_string(command)
                    : jsc_value_to_json(value, 0);
  if (command) g_object_unref(command);
  if (!json) {
    set_status(state, "Roblox sent an unreadable launch request");
    return;
  }
  const std::string request(json);
  g_free(json);
  if (send_to_supervisor(state, request)) {
    set_status(state, "Sent Roblox Hybrid command to the runtime…");
    return;
  }
  set_status(state, "No Nuah runtime supervisor is connected");
}

void on_sign_in_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  gtk_stack_set_visible_child_name(state->stack, "login");
  if (state->session_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->session_button), false);
  }
  webkit_web_view_load_uri(state->web_view, "https://www.roblox.com/login");
  /* WebKit is the interactive child of the login page.  Grab focus after
   * the stack transition so the first click/keypress is not consumed by the
   * outgoing welcome page. */
  g_idle_add(
      [](gpointer user_data) -> gboolean {
        auto* service = static_cast<ServiceState*>(user_data);
        gtk_widget_grab_focus(GTK_WIDGET(service->web_view));
        return G_SOURCE_REMOVE;
      },
      state);
}

void on_home_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (state->web_view) webkit_web_view_load_uri(state->web_view, "https://www.roblox.com/home");
}

void on_discover_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (state->web_view) webkit_web_view_load_uri(state->web_view, "https://www.roblox.com/discover");
}

void on_reload_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (state->web_view) webkit_web_view_reload(state->web_view);
}

void on_search_activated(GtkSearchEntry* entry, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (!state->web_view) return;
  const char* raw = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (!raw || !*raw) return;
  std::string text(raw);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  if (text.empty()) return;

  if (text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0) {
    webkit_web_view_load_uri(state->web_view, text.c_str());
    return;
  }
  if (text.rfind("roblox.com", 0) == 0 || text.rfind("www.roblox.com", 0) == 0) {
    webkit_web_view_load_uri(state->web_view, ("https://" + text).c_str());
    return;
  }

  bool is_all_digits = true;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      is_all_digits = false;
      break;
    }
  }
  if (is_all_digits) {
    webkit_web_view_load_uri(state->web_view, ("https://www.roblox.com/games/" + text + "/").c_str());
    return;
  }

  gchar* encoded = g_uri_escape_string(text.c_str(), nullptr, false);
  const std::string search_url = "https://www.roblox.com/discover/?Keyword=" + std::string(encoded ? encoded : text.c_str());
  if (encoded) g_free(encoded);
  webkit_web_view_load_uri(state->web_view, search_url.c_str());
}

void on_back_clicked(GtkButton*, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  gtk_stack_set_visible_child_name(state->stack, "login");
  webkit_web_view_load_uri(state->web_view, "https://www.roblox.com/home");
}

GtkWidget* build_welcome(ServiceState* state) {
  auto* clamp = adw_clamp_new();
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(box, 72);
  gtk_widget_set_margin_bottom(box, 48);
  gtk_widget_set_margin_start(box, 32);
  gtk_widget_set_margin_end(box, 32);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);

  auto* icon = gtk_image_new_from_icon_name("applications-games-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 72);
  gtk_box_append(GTK_BOX(box), icon);
  auto* title = gtk_label_new("Play Roblox with Nuah");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);
  auto* description = gtk_label_new(
      "Sign in in Nuah's Roblox web view, then continue into the Roblox "
      "Android client. Nuah transfers only the authenticated Roblox session "
      "to its private Android WebView store.");
  gtk_label_set_wrap(GTK_LABEL(description), true);
  gtk_label_set_justify(GTK_LABEL(description), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(description, "dim-label");
  gtk_box_append(GTK_BOX(box), description);

  state->play_button = GTK_BUTTON(gtk_button_new_with_label("Open Roblox"));
  gtk_widget_add_css_class(GTK_WIDGET(state->play_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(state->play_button), "pill");
  gtk_widget_set_halign(GTK_WIDGET(state->play_button), GTK_ALIGN_CENTER);
  g_signal_connect(
      state->play_button, "clicked", G_CALLBACK(on_play_clicked), state);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->play_button));

  auto* web_page = gtk_button_new_with_label("Open Roblox web page");
  gtk_widget_add_css_class(web_page, "pill");
  gtk_widget_set_halign(web_page, GTK_ALIGN_CENTER);
  g_signal_connect(
      web_page, "clicked", G_CALLBACK(on_sign_in_clicked), state);
  gtk_box_append(GTK_BOX(box), web_page);

  auto* use_session = gtk_button_new_with_label("Use browser session in Roblox");
  gtk_widget_add_css_class(use_session, "pill");
  gtk_widget_set_halign(use_session, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(
      use_session,
      "Transfers only the Roblox session cookie into Android WebView after you sign in");
  g_signal_connect(use_session, "clicked",
                   G_CALLBACK(on_use_browser_session_clicked), state);
  gtk_box_append(GTK_BOX(box), use_session);

  state->status = GTK_LABEL(gtk_label_new(state->package.message.c_str()));
  gtk_label_set_wrap(state->status, true);
  gtk_label_set_justify(state->status, GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(state->status), "dim-label");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->status));
  return clamp;
}

GtkWidget* build_login(ServiceState* state) {
  auto* toolbar = adw_toolbar_view_new();
  auto* header = adw_header_bar_new();

  auto* nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  auto* back = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_set_tooltip_text(back, "Home / Back");
  g_signal_connect(back, "clicked", G_CALLBACK(on_back_clicked), state);
  gtk_box_append(GTK_BOX(nav_box), back);

  auto* home = gtk_button_new_from_icon_name("user-home-symbolic");
  gtk_widget_set_tooltip_text(home, "Roblox Home");
  g_signal_connect(home, "clicked", G_CALLBACK(on_home_clicked), state);
  gtk_box_append(GTK_BOX(nav_box), home);

  auto* discover = gtk_button_new_from_icon_name("compass-symbolic");
  gtk_widget_set_tooltip_text(discover, "Discover Games");
  g_signal_connect(discover, "clicked", G_CALLBACK(on_discover_clicked), state);
  gtk_box_append(GTK_BOX(nav_box), discover);

  auto* reload = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(reload, "Reload");
  g_signal_connect(reload, "clicked", G_CALLBACK(on_reload_clicked), state);
  gtk_box_append(GTK_BOX(nav_box), reload);

  adw_header_bar_pack_start(ADW_HEADER_BAR(header), nav_box);

  state->session_button = GTK_BUTTON(
      gtk_button_new_with_label("Logout"));
  gtk_widget_set_sensitive(GTK_WIDGET(state->session_button), false);
  gtk_widget_add_css_class(GTK_WIDGET(state->session_button),
                           "destructive-action");
  gtk_widget_set_tooltip_text(
      GTK_WIDGET(state->session_button),
      "Log out and clear saved Roblox cookies");
  g_signal_connect(state->session_button, "clicked",
                   G_CALLBACK(on_logout_clicked), state);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header),
                          GTK_WIDGET(state->session_button));

  state->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_search_entry_set_placeholder_text(state->search_entry,
                                        "Search games or enter Place ID…");
  gtk_widget_set_size_request(GTK_WIDGET(state->search_entry), 360, -1);
  g_signal_connect(state->search_entry, "activate",
                   G_CALLBACK(on_search_activated), state);
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                  GTK_WIDGET(state->search_entry));

  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

  state->bridge = webkit_user_content_manager_new();
  webkit_user_content_manager_register_script_message_handler(
      state->bridge, "executeRoblox", nullptr);
  webkit_user_content_manager_register_script_message_handler(
      state->bridge, "RobloxWKHybrid", nullptr);
  g_signal_connect(
      state->bridge, "script-message-received::executeRoblox",
      G_CALLBACK(on_script_message), state);
  g_signal_connect(
      state->bridge, "script-message-received::RobloxWKHybrid",
      G_CALLBACK(on_script_message), state);
  auto* bridge_script = webkit_user_script_new(
      R"JS(
window.__globalRobloxAndroidBridge__={executeRoblox:(query)=>{
  const json=typeof query==='string'?JSON.parse(query):query;
  window.webkit.messageHandlers.executeRoblox.postMessage(json);
}};

// In-app Hybrid pages leave #game-details-play-button-container empty:
// Android draws Play in the native chrome. WebKit has no such chrome, so
// fill that same slot with Roblox's own play-button classes. Do not create
// window.Roblox before their scripts; that can skip their bootstrap.
(()=>{
  if(window.top!==window) return;
  const LAUNCH_MODES={
    SIMPLE_GAME:'SimpleGame',
    GAME_INSTANCE:'GameInstance',
    FOLLOW_USER:'FollowUser',
    PRIVATE_SERVER:'PrivateServer'
  };
  const post=(payload)=>{
    window.__globalRobloxAndroidBridge__.executeRoblox(JSON.stringify(payload));
  };
  const launchGame=(params,callback)=>{
    const src=(params&&params.request)?params.request:(params||{});
    const request={};
    if(src.placeId!=null){
      const placeId=Number(src.placeId);
      request.placeId=placeId;
      request.rootPlaceId=Number(src.rootPlaceId!=null?src.rootPlaceId:placeId);
    }
    if(src.instanceId) request.gameInstanceId=String(src.instanceId);
    if(src.gameId && !request.gameInstanceId) request.gameInstanceId=String(src.gameId);
    if(src.accessCode) request.reservedServerAccessCode=String(src.accessCode);
    if(src.userId) request.playerId=String(src.userId);
    if(src.joinAttemptId) request.joinAttemptId=String(src.joinAttemptId);
    if(src.joinAttemptOrigin) request.joinAttemptOrigin=String(src.joinAttemptOrigin);
    const callbackID=(typeof crypto!=='undefined'&&crypto.randomUUID)
      ?crypto.randomUUID():String(Date.now());
    post({
      moduleID:'Game',
      functionName:'launchGame',
      params:{request},
      callbackID
    });
    if(typeof callback==='function') callback();
  };
  const attachHybrid=(root)=>{
    if(!root||typeof root!=='object') return root;
    const hybrid=root.Hybrid||(root.Hybrid={});
    hybrid.Game=hybrid.Game||{};
    hybrid.Game.LAUNCH_MODES=hybrid.Game.LAUNCH_MODES||LAUNCH_MODES;
    if(typeof hybrid.Game.launchGame!=='function')
      hybrid.Game.launchGame=launchGame;
    if(typeof hybrid.Game.startWithPlaceID!=='function')
      hybrid.Game.startWithPlaceID=(placeId,callback)=>launchGame({placeId},callback);
    hybrid.Bridge=hybrid.Bridge||{};
    if(typeof hybrid.Bridge.nativeCallback!=='function')
      hybrid.Bridge.nativeCallback=function(){};
    return root;
  };
  let roblox=window.Roblox;
  if(roblox) attachHybrid(roblox);
  try{
    Object.defineProperty(window,'Roblox',{
      configurable:true,
      enumerable:true,
      get(){return roblox;},
      set(value){roblox=attachHybrid(value);}
    });
  }catch(e){}
  const fillPlay=()=>{
    if(!location.hostname.endsWith('roblox.com')) return;
    const match=location.pathname.match(/^\/games\/(\d+)/);
    if(!match) return;
    const box=document.getElementById('game-details-play-button-container');
    if(!box) return;
    const native=box.querySelector(
      '[data-testid="play-button"]:not([data-nuah-play]), .VisitButtonPlay, .VisitButtonPlayGLI, .btn-common-play-game-lg:not([data-nuah-play])');
    if(native){
      const ours=box.querySelector('[data-nuah-play]');
      if(ours) ours.remove();
      return;
    }
    if(box.querySelector('[data-nuah-play]')) return;
    if(box.querySelector('.spinner') && !box.dataset.nuahPlayReady) return;
    const placeId=Number(match[1]);
    const btn=document.createElement('button');
    btn.type='button';
    btn.dataset.nuahPlay='1';
    btn.dataset.testid='play-button';
    btn.className='btn-common-play-game-lg btn-full-width';
    btn.setAttribute('aria-label','Play');
    btn.innerHTML='<span class="icon-common-play"></span><span class="play-button-text">Play</span>';
    btn.addEventListener('click',(event)=>{
      event.preventDefault();
      event.stopPropagation();
      launchGame({placeId:placeId,rootPlaceId:placeId,requestType:LAUNCH_MODES.SIMPLE_GAME});
    });
    box.replaceChildren(btn);
  };
  const watchPlay=()=>{
    fillPlay();
    const box=document.getElementById('game-details-play-button-container');
    if(!box) return;
    window.setTimeout(()=>{
      box.dataset.nuahPlayReady='1';
      fillPlay();
    },1800);
    new MutationObserver(fillPlay).observe(box,{childList:true,subtree:true});
  };
  if(document.readyState==='loading')
    document.addEventListener('DOMContentLoaded',watchPlay,{once:true});
  else watchPlay();
})();
)JS",
      WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(state->bridge, bridge_script);
  webkit_user_script_unref(bridge_script);
  state->web_view = WEBKIT_WEB_VIEW(g_object_new(
      WEBKIT_TYPE_WEB_VIEW, "user-content-manager", state->bridge, nullptr));
  WebKitSettings* settings = webkit_web_view_get_settings(state->web_view);
  webkit_settings_set_enable_javascript(settings, true);
  webkit_settings_set_enable_smooth_scrolling(settings, true);
  webkit_settings_set_hardware_acceleration_policy(
      settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
  webkit_settings_set_enable_write_console_messages_to_stdout(settings, false);
  webkit_settings_set_user_agent(
      settings,
      "Mozilla/5.0 AppleWebKit/605.1.15 (KHTML, like Gecko) "
      "ROBLOX Android App 2.734.790 Tablet Hybrid() GooglePlayStore "
      "RobloxApp/2.734.790 "
      "(GlobalDist; GooglePlayStore)");
  auto* cookie_manager = webkit_network_session_get_cookie_manager(
      webkit_web_view_get_network_session(state->web_view));
  const auto webkit_directory = state->data_directory / "webkit";
  std::filesystem::create_directories(webkit_directory);
  const auto cookie_database = webkit_directory / "cookies.sqlite";
  webkit_cookie_manager_set_persistent_storage(
      cookie_manager, cookie_database.c_str(),
      WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  webkit_cookie_manager_set_accept_policy(
      cookie_manager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
  g_signal_connect(
      state->web_view, "load-changed", G_CALLBACK(on_load_changed), state);
  g_signal_connect(state->web_view, "decide-policy",
                   G_CALLBACK(on_decide_policy), state);
  gtk_widget_set_focusable(GTK_WIDGET(state->web_view), true);
  gtk_widget_set_focus_on_click(GTK_WIDGET(state->web_view), true);
  gtk_widget_set_can_target(GTK_WIDGET(state->web_view), true);
  gtk_widget_set_hexpand(GTK_WIDGET(state->web_view), true);
  gtk_widget_set_vexpand(GTK_WIDGET(state->web_view), true);
  adw_toolbar_view_set_content(
      ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(state->web_view));
  return toolbar;
}

gboolean on_window_close(GtkWindow* window, gpointer data) {
  (void)window;
  (void)data;
  return false;
}

gboolean on_server_frame(gint, GIOCondition condition, gpointer data) {
  auto* state = static_cast<ServiceState*>(data);
  if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
    state->server_fd = -1;
    set_status(state, "Runtime supervisor disconnected");
    return G_SOURCE_REMOVE;
  }
  nuah::SoberRecord record{};
  std::string error;
  if (!nuah::receive_sober_record(state->server_fd, record, error)) {
    state->server_fd = -1;
    set_status(state, error.c_str());
    return G_SOURCE_REMOVE;
  }
  if (record.opcode == nuah::kSoberLoadUri) {
    if (record.payload.empty()) return G_SOURCE_CONTINUE;
    gtk_stack_set_visible_child_name(state->stack, "login");
    webkit_web_view_load_uri(state->web_view, record.payload.c_str());
    gtk_window_present(state->window);
  } else if (record.opcode == nuah::kSoberSetTitle) {
    gtk_window_set_title(state->window, record.payload.c_str());
  } else if (record.opcode == nuah::kSoberSetVisible) {
    const bool visible = !record.payload.empty() && record.payload[0] != 0;
    gtk_widget_set_visible(GTK_WIDGET(state->window), visible);
    if (visible) gtk_window_present(state->window);
  } else if (record.opcode == nuah::kSoberEvaluateJavaScript) {
    if (record.payload.empty()) return G_SOURCE_CONTINUE;
    webkit_web_view_evaluate_javascript(
        state->web_view, record.payload.c_str(),
        static_cast<gssize>(record.payload.size()),
        nullptr, nullptr, nullptr, javascript_finished, state);
  } else if (record.opcode == nuah::kSoberQuit) {
    g_application_quit(state->app);
  }
  return G_SOURCE_CONTINUE;
}

void on_activate(GApplication* app, gpointer data) {
  const auto* launch = static_cast<const ServiceLaunch*>(data);
  auto* state = new ServiceState();
  state->app = app;
  state->server_fd = launch ? launch->server_fd : -1;
  state->data_directory = data_directory();
  state->package = nuah::inspect_sober_cache();
  state->window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  gtk_window_set_title(state->window, "Nuah");
  gtk_window_set_default_size(state->window, 920, 720);
  // Preserve the compositor activation token supplied by the desktop
  // launcher. Without it, a Wayland compositor may create Main behind the
  // currently focused application, making the otherwise-live WebKit UI look
  // unresponsive.
  if (const char* startup_id = std::getenv("DESKTOP_STARTUP_ID");
      startup_id && *startup_id) {
    gtk_window_set_startup_id(state->window, startup_id);
  }

  state->stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(
      state->stack, GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_stack_add_named(state->stack, build_welcome(state), "welcome");
  gtk_stack_add_named(state->stack, build_login(state), "login");
  // Services is the Roblox browser shell.  The Android runtime is only
  // created after a game join, so never expose the custom welcome panel as
  // the primary surface.
  gtk_stack_set_visible_child_name(state->stack, "login");
  adw_application_window_set_content(
      ADW_APPLICATION_WINDOW(state->window), GTK_WIDGET(state->stack));
  g_signal_connect(
      state->window, "close-request", G_CALLBACK(on_window_close), state);

  g_object_set_data_full(G_OBJECT(state->window), "nuah-service-state", state,
                         [](gpointer data) {
                           auto* value = static_cast<ServiceState*>(data);
                           if (value->server_fd >= 0) {
                             ::close(value->server_fd);
                           }
                           g_clear_object(&value->bridge);
                           delete value;
                         });
  gtk_window_present(state->window);
  if (state->server_fd < 0) {
    webkit_web_view_load_uri(state->web_view, "https://www.roblox.com/home");
  }
  if (state->server_fd >= 0) {
    g_unix_fd_add_full(G_PRIORITY_DEFAULT, state->server_fd, G_IO_IN,
                       on_server_frame, state, nullptr);
  }
}

int run_probe(int fd) {
  nuah::Frame frame{};
  std::string error;
  if (!nuah::receive_frame(fd, frame, error)) {
    std::cerr << "nuah-services: " << error << '\n';
    return 1;
  }
  std::cout << "nuah-services received request " << frame.request_id << ": "
            << frame.json << '\n';
  const nuah::Frame reply{
      nuah::Opcode::event, frame.request_id, R"({"type":"service_ready"})"};
  if (!nuah::send_frame(fd, reply, error)) {
    std::cerr << "nuah-services: " << error << '\n';
    return 1;
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--fd") {
    return run_probe(std::atoi(argv[2]));
  }
  ServiceLaunch launch;
  if (argc == 4 && std::string(argv[1]) == "--server") {
    launch.server_fd = std::atoi(argv[2]);
    launch.detached = std::string(argv[3]) == "detached";
    if (launch.server_fd < 0 ||
        (std::string(argv[3]) != "attached" && !launch.detached)) {
      std::cerr << "usage: nuah-services --server <inherited-socket> attached|detached\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "usage: nuah-services [--fd <inherited-socket>] [--server <inherited-socket> attached|detached]\n";
    return 2;
  }
  // A supervised helper is addressed by its inherited socket, not by an
  // existing desktop instance with the same application ID.  Without this,
  // GApplication forwards --server to a standalone Nuah window and the
  // supervisor loses its handshake.
  const auto flags = launch.server_fd >= 0
                         ? G_APPLICATION_NON_UNIQUE
                         : G_APPLICATION_DEFAULT_FLAGS;
  auto* app = adw_application_new("org.nuah.Nuah", flags);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), &launch);
  // --server is Nuah's private inherited-FD argument.  Do not present it to
  // GApplication's option parser, which quite correctly does not know it.
  char* application_argv[] = {argv[0], nullptr};
  const int status = g_application_run(G_APPLICATION(app), 1, application_argv);
  g_object_unref(app);
  return status;
}

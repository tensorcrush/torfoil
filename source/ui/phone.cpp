#include "ui/phone.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "net/http_parse.hpp"
#include "ui/lang.hpp"
#include "util/bytes.hpp"
#include "util/log.hpp"

namespace ui {

namespace {

// La page. Un seul fichier, aucune ressource externe : le téléphone n'a pas
// forcément d'accès Internet sur ce réseau, et une feuille de style qui ne se
// charge pas donnerait une page inutilisable.
//
// Écrite pour un écran de téléphone tenu à la main : cibles tactiles larges,
// une action par bloc, et l'ajout en tête — c'est ce pour quoi on vient.
const char kPage[] = R"PAGE(<!doctype html>
<html lang="$LANG$">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#14171c">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>Torfoil</title>
<style>
:root{--bg:#14171c;--card:#1e222a;--alt:#262b35;--txt:#e8ecf1;--dim:#8a93a3;
--accent:#3dd6c4;--accent-dim:#1f6b63;--warn:#f2b13c;--err:#e55b5b;--ok:#63c96a}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--txt);
font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
padding:0 16px calc(32px + env(safe-area-inset-bottom))}
header{position:sticky;top:0;background:var(--bg);padding:18px 0 12px;z-index:5;
display:flex;align-items:baseline;gap:12px;border-bottom:1px solid var(--alt)}
h1{margin:0;font-size:22px;letter-spacing:.5px}
h1 span{color:var(--accent)}
#rates{margin-left:auto;font-size:13px;color:var(--dim);font-variant-numeric:tabular-nums}
section{background:var(--card);border-radius:14px;padding:16px;margin-top:14px}
h2{margin:0 0 12px;font-size:13px;text-transform:uppercase;letter-spacing:1.2px;color:var(--dim)}
.drop{display:block;text-align:center;padding:26px 16px;border:2px dashed var(--accent-dim);
border-radius:12px;color:var(--accent);font-weight:600;cursor:pointer}
.drop:active{background:var(--alt)}
textarea{width:100%;background:var(--alt);color:var(--txt);border:0;border-radius:10px;
padding:12px;font:15px/1.4 ui-monospace,Menlo,monospace;resize:vertical}
button{-webkit-appearance:none;appearance:none;border:0;border-radius:10px;
background:var(--accent-dim);color:var(--txt);font:600 16px/1 inherit;
padding:14px 18px;width:100%;margin-top:10px;cursor:pointer}
button:active{background:var(--accent);color:#0d1113}
.hint{margin:10px 0 0;font-size:13px;color:var(--dim)}
#flash{margin-top:14px;padding:14px 16px;border-radius:12px;font-size:15px;display:none}
#flash.ok{display:block;background:var(--accent-dim)}
#flash.err{display:block;background:var(--err)}
.t{padding:14px 0;border-top:1px solid var(--alt)}
.t:first-child{border-top:0;padding-top:0}
.t .n{font-weight:600;word-break:break-word}
.t .d{font-size:13px;color:var(--dim);margin-top:3px;font-variant-numeric:tabular-nums}
.bar{height:6px;border-radius:3px;background:var(--alt);margin-top:9px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--accent);border-radius:3px}
.acts{display:flex;gap:8px;margin-top:10px}
.acts button{width:auto;flex:1;padding:9px 0;font-size:14px;background:var(--alt);margin:0}
.empty{color:var(--dim);font-size:14px}
</style>
</head>
<body>
<header><h1>Tor<span>foil</span></h1><div id="rates">…</div></header>

<section>
  <h2>$FILE_SECTION$</h2>
  <label class="drop" for="f">$FILE_PICK$</label>
  <input id="f" type="file" accept=".torrent,application/x-bittorrent" multiple hidden>
  <p class="hint">$FILE_HINT$</p>
</section>

<section>
  <h2>$MAGNET_SECTION$</h2>
  <textarea id="m" rows="3" placeholder="magnet:?xt=urn:btih:…"
    autocapitalize="off" autocorrect="off" autocomplete="off" spellcheck="false"></textarea>
  <button id="go">$ADD$</button>
  <p class="hint">$MAGNET_HINT$</p>
</section>

<div id="flash"></div>

<section>
  <h2>$ON_CONSOLE$</h2>
  <div id="list"><p class="empty">…</p></div>
</section>

<script>
var flash = document.getElementById('flash');
function say(msg, err) {
  flash.textContent = msg;
  flash.className = err ? 'err' : 'ok';
  clearTimeout(say.t);
  say.t = setTimeout(function(){ flash.className = ''; }, 6000);
}

function send(form) {
  say($SENDING$, false);
  fetch('/api/add', {method:'POST', body:form})
    .then(function(r){ return r.json(); })
    .then(function(j){ say(j.message, !j.ok); refresh(); })
    .catch(function(){ say($NO_ANSWER$, true); });
}

document.getElementById('f').addEventListener('change', function(e) {
  if (!e.target.files.length) return;
  var form = new FormData();
  for (var i = 0; i < e.target.files.length; i++) form.append('torrent', e.target.files[i]);
  send(form);
  e.target.value = '';
});

document.getElementById('go').addEventListener('click', function() {
  var box = document.getElementById('m');
  var text = box.value.trim();
  if (!text) { say($NOTHING$, true); return; }
  var form = new FormData();
  form.append('magnet', text);
  send(form);
  box.value = '';
  box.blur();
});

function act(hash, op) {
  if (op === 'remove' && !confirm($CONFIRM_REMOVE$)) return;
  fetch('/api/action', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'op=' + op + '&hash=' + hash
  }).then(function(r){ return r.json(); })
    .then(function(j){ say(j.message, !j.ok); refresh(); });
}

function draw(s) {
  document.getElementById('rates').textContent = '↓ ' + s.down + '   ↑ ' + s.up;
  var list = document.getElementById('list');
  if (!s.torrents.length) {
    list.innerHTML = '<p class="empty">' + $EMPTY_LIST$ + '</p>';
    return;
  }
  var html = '';
  for (var i = 0; i < s.torrents.length; i++) {
    var t = s.torrents[i];
    var pct = Math.round(t.progress * 100);
    html += '<div class="t"><div class="n">' + t.name + '</div>'
         +  '<div class="d">' + t.state + ' · ' + pct + ' % · ' + t.size
         +  ' · ' + t.peers + ' · ↓ ' + t.rate + (t.eta ? ' · ' + t.eta : '') + '</div>'
         +  '<div class="bar"><i style="width:' + pct + '%"></i></div>'
         +  (t.message ? '<div class="d">' + t.message + '</div>' : '')
         +  '<div class="acts">'
         +  '<button onclick="act(\'' + t.hash + '\',\'' + (t.paused ? 'resume' : 'pause') + '\')">'
         +  (t.paused ? $RESUME$ : $PAUSE$) + '</button>'
         +  '<button onclick="act(\'' + t.hash + '\',\'remove\')">' + $REMOVE$ + '</button>'
         +  '</div></div>';
  }
  list.innerHTML = html;
}

var timer = null;
function refresh() {
  fetch('/api/status').then(function(r){ return r.json(); }).then(draw).catch(function(){});
}
function loop() {
  clearInterval(timer);
  // Inutile d'interroger la console pendant que la page est en arrière-plan :
  // sur iOS l'onglet est gelé et les requêtes s'accumulent pour rien.
  if (!document.hidden) { refresh(); timer = setInterval(refresh, 2000); }
}
document.addEventListener('visibilitychange', loop);
loop();
</script>
</body>
</html>
)PAGE";

// Un texte destiné à du JavaScript, guillemets compris : il finit dans une
// expression, pas dans du HTML, et un simple échappement HTML ne l'y protégerait
// pas. Les traductions n'ont aucune raison de contenir de quoi casser la page,
// mais on ne construit pas du code avec des données sans les échapper.
Str state_key(bt::TorrentState state) {
    switch (state) {
        case bt::TorrentState::FetchingMetadata: return Str::StateMetadata;
        case bt::TorrentState::Checking: return Str::StateChecking;
        case bt::TorrentState::Downloading: return Str::StateDownloading;
        case bt::TorrentState::Seeding: return Str::StateSeeding;
        case bt::TorrentState::Paused: return Str::StatePaused;
        case bt::TorrentState::Completed: return Str::StateCompleted;
        case bt::TorrentState::Failed: return Str::StateFailed;
    }
    return Str::StateMetadata;
}

std::string js_literal(Str key) {
    return "'" + net::json_escape(tr(key)) + "'";
}

// La page est assemblée à la volée dans la langue de la console : le téléphone
// et la console appartiennent à la même personne, et c'est elle qui a choisi.
std::string render_page() {
    static const struct {
        const char* token;
        Str key;
        bool as_js;
    } kSlots[] = {
        {"$FILE_SECTION$", Str::WebFileSection, false},
        {"$FILE_PICK$", Str::WebFilePick, false},
        {"$FILE_HINT$", Str::WebFileHint, false},
        {"$MAGNET_SECTION$", Str::WebMagnetSection, false},
        {"$ADD$", Str::WebAdd, false},
        {"$MAGNET_HINT$", Str::WebMagnetHint, false},
        {"$ON_CONSOLE$", Str::WebOnConsole, false},
        {"$SENDING$", Str::WebSending, true},
        {"$NO_ANSWER$", Str::WebNoAnswer, true},
        {"$NOTHING$", Str::WebNothingToAdd, true},
        {"$CONFIRM_REMOVE$", Str::WebConfirmRemove, true},
        {"$EMPTY_LIST$", Str::WebEmptyList, true},
        {"$RESUME$", Str::WebResume, true},
        {"$PAUSE$", Str::WebPause, true},
        {"$REMOVE$", Str::WebRemove, true},
    };

    std::string page = kPage;
    {
        const std::string token = "$LANG$";
        const size_t pos = page.find(token);
        if (pos != std::string::npos) page.replace(pos, token.size(), code_of(language()));
    }

    for (const auto& slot : kSlots) {
        const std::string value = slot.as_js ? js_literal(slot.key)
                                             : net::html_escape(tr(slot.key));
        size_t pos = 0;
        const size_t token_len = std::strlen(slot.token);
        while ((pos = page.find(slot.token, pos)) != std::string::npos) {
            page.replace(pos, token_len, value);
            pos += value.size();
        }
    }
    return page;
}

net::HttpReply json_reply(bool ok, const std::string& message) {
    net::HttpReply reply;
    reply.status = 200;
    reply.content_type = "application/json; charset=utf-8";
    reply.body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" +
                 net::json_escape(message) + "\"}";
    return reply;
}

}  // namespace

bool PhoneBridge::start(bt::Session& session, const std::string& inbox_dir, uint16_t port,
                        std::string* err) {
    session_ = &session;
    inbox_dir_ = inbox_dir;
    ::mkdir(inbox_dir_.c_str(), 0777);

    if (!server_.start(port, [this](const net::HttpRequest& r) { return dispatch(r); }, err)) {
        return false;
    }

    url_ = server_.url();
    if (url_.empty()) {
        // Le serveur écoute, mais la console n'a pas d'adresse : sans réseau
        // local, il n'y a rien à afficher et rien à viser. On le dit plutôt que
        // de laisser un code QR pointer vers une adresse inexistante.
        server_.stop();
        if (err) *err = tr(Str::RemoteNoLan);
        return false;
    }

    if (!util::qr_encode(url_, qr_)) qr_ = util::QrCode{};
    util::log_line("import téléphone : " + url_);
    return true;
}

void PhoneBridge::stop() {
    server_.stop();
    url_.clear();
    qr_ = util::QrCode{};
}

std::string PhoneBridge::last_error() const {
    return server_.last_error();
}

void PhoneBridge::notice(const std::string& message, bool error) {
    std::lock_guard<std::mutex> lock(mutex_);
    notice_ = message;
    notice_error_ = error;
    notice_pending_ = true;
}

bool PhoneBridge::take_notice(std::string& message, bool& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!notice_pending_) return false;
    message = notice_;
    error = notice_error_;
    notice_pending_ = false;
    return true;
}

void PhoneBridge::save_copy(const std::string& filename, const std::string& blob) const {
    if (inbox_dir_.empty()) return;
    const std::string clean = util::sanitize_filename(filename);
    if (clean.empty()) return;

    const std::string path = inbox_dir_ + "/" + clean;
    if (std::FILE* fp = std::fopen(path.c_str(), "wb")) {
        std::fwrite(blob.data(), 1, blob.size(), fp);
        std::fclose(fp);
    }
}

net::HttpReply PhoneBridge::dispatch(const net::HttpRequest& request) {
    if (request.path == "/" || request.path == "/index.html") {
        net::HttpReply reply;
        reply.body = render_page();
        return reply;
    }
    if (request.path == "/api/status") return {200, "application/json; charset=utf-8",
                                               status_json(), {}};
    if (request.path == "/api/add") {
        if (request.method != "POST") return json_reply(false, tr(Str::WebBadMethod));
        return handle_add(request);
    }
    if (request.path == "/api/action") {
        if (request.method != "POST") return json_reply(false, tr(Str::WebBadMethod));
        return handle_action(request);
    }
    if (request.path == "/favicon.ico") return {204, "image/png", {}, {}};

    net::HttpReply reply;
    reply.status = 404;
    reply.content_type = "text/plain; charset=utf-8";
    reply.body = "introuvable";
    return reply;
}

net::HttpReply PhoneBridge::handle_add(const net::HttpRequest& request) {
    if (!session_) return json_reply(false, "session indisponible");

    std::vector<net::MultipartPart> parts;
    if (!net::parse_multipart(request.content_type(), request.body, parts)) {
        return json_reply(false, tr(Str::WebUnreadable));
    }

    int added = 0;
    std::vector<std::string> refused;
    std::string last_name;

    for (const net::MultipartPart& part : parts) {
        if (part.name == "magnet") {
            // Le champ peut contenir plusieurs liens, un par ligne : coller une
            // liste entière est le cas d'usage normal quand on prépare ses
            // téléchargements sur le téléphone.
            for (const std::string& raw : util::split(part.data, '\n')) {
                const std::string uri = util::trim(raw);
                if (uri.empty()) continue;

                std::string err;
                if (session_->add_magnet(uri, &err)) {
                    ++added;
                    last_name = uri.size() > 60 ? uri.substr(0, 57) + "…" : uri;
                } else {
                    refused.push_back(err);
                }
            }
            continue;
        }

        if (part.data.empty()) continue;

        std::string err;
        std::string name;
        if (session_->add_torrent_data(part.data, &err, &name)) {
            ++added;
            last_name = name.empty() ? part.filename : name;
            save_copy(part.filename.empty() ? name + ".torrent" : part.filename, part.data);
        } else {
            // On nomme le fichier fautif : avec plusieurs envois d'un coup, une
            // raison sans nom ne dit pas lequel refaire.
            refused.push_back((part.filename.empty() ? std::string("fichier") : part.filename) +
                              " : " + err);
        }
    }

    std::string message;
    if (added > 0) {
        added_.fetch_add(static_cast<uint32_t>(added));
        message = added > 1 ? trf(Str::ToastTorrentsAdded, added) : tr(Str::ToastTorrentAdded);
        notice(trf(Str::ToastFromPhone,
                   (added == 1 && !last_name.empty() ? last_name : message).c_str()),
               false);
    }
    if (!refused.empty()) {
        if (!message.empty()) message += " · ";
        message += trf(Str::WebRefused, static_cast<int>(refused.size()));
        message += " (" + refused.front() + ")";
        if (added == 0) notice(trf(Str::ToastFromPhone, refused.front().c_str()), true);
    }
    if (message.empty()) return json_reply(false, tr(Str::WebNothingReceived));

    util::log_line("import téléphone : " + message);
    return json_reply(added > 0, message);
}

net::HttpReply PhoneBridge::handle_action(const net::HttpRequest& request) {
    if (!session_) return json_reply(false, "session indisponible");

    const std::string op = net::form_field(request.body, "op");
    const std::string hash = net::form_field(request.body, "hash");
    if (hash.empty()) return json_reply(false, tr(Str::WebNoTorrentGiven));

    if (op == "pause") {
        session_->pause(hash);
        return json_reply(true, tr(Str::ToastPaused));
    }
    if (op == "resume") {
        session_->resume(hash);
        return json_reply(true, tr(Str::ToastResumed));
    }
    if (op == "remove") {
        // Jamais de suppression de fichiers depuis le téléphone : l'écran de la
        // console est le seul endroit où l'on voit ce qu'on détruit.
        session_->remove(hash, /*delete_files=*/false);
        return json_reply(true, tr(Str::ToastRemovedKept));
    }
    return json_reply(false, tr(Str::WebUnknownAction));
}

std::string PhoneBridge::status_json() const {
    if (!session_) return "{\"down\":\"0\",\"up\":\"0\",\"torrents\":[]}";

    std::string out = "{\"down\":\"" + util::human_rate(session_->rate_down()) + "\",\"up\":\"" +
                      util::human_rate(session_->rate_up()) + "\",\"torrents\":[";

    bool first = true;
    for (const bt::TorrentStatus& t : session_->snapshot()) {
        if (!first) out += ",";
        first = false;

        char progress[32];
        std::snprintf(progress, sizeof(progress), "%.4f", static_cast<double>(t.progress));

        out += "{\"hash\":\"" + net::json_escape(t.hash_hex) + "\"";
        out += ",\"name\":\"" + net::json_escape(net::html_escape(t.name)) + "\"";
        out += ",\"state\":\"" + net::json_escape(tr(state_key(t.state))) + "\"";
        out += ",\"progress\":" + std::string(progress);
        out += ",\"size\":\"" +
               (t.total_size ? util::human_size(t.total_size) : std::string(tr(Str::SizeUnknown))) +
               "\"";
        out += ",\"rate\":\"" + util::human_rate(t.rate_down) + "\"";
        out += ",\"peers\":\"" +
               net::json_escape(trf(Str::PeersPlain, t.peers_connected)) + "\"";
        out += ",\"eta\":\"" +
               (t.eta_s > 0 && t.state == bt::TorrentState::Downloading
                    ? util::human_duration(t.eta_s)
                    : std::string()) +
               "\"";
        out += ",\"paused\":" + std::string(t.state == bt::TorrentState::Paused ? "true" : "false");
        out += ",\"message\":\"" + net::json_escape(net::html_escape(t.message)) + "\"}";
    }
    out += "]}";
    return out;
}

}  // namespace ui

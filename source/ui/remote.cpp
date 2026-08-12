#include "ui/remote.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "net/http_parse.hpp"
#include "net/json.hpp"
#include "util/bytes.hpp"
#include "util/log.hpp"

namespace ui {

namespace {

std::string make_token() {
    uint8_t raw[4] = {0, 0, 0, 0};
    util::random_bytes(raw, sizeof(raw));
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : raw) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0f];
    }
    return out;
}

const char* state_name(bt::TorrentState state) {
    switch (state) {
        case bt::TorrentState::FetchingMetadata: return "metadata";
        case bt::TorrentState::Checking: return "checking";
        case bt::TorrentState::Downloading: return "downloading";
        case bt::TorrentState::Seeding: return "seeding";
        case bt::TorrentState::Paused: return "paused";
        case bt::TorrentState::Queued: return "queued";
        case bt::TorrentState::Completed: return "completed";
        case bt::TorrentState::Failed: return "failed";
    }
    return "unknown";
}

net::HttpReply json_reply(const std::string& body, int status = 200) {
    net::HttpReply reply;
    reply.status = status;
    reply.content_type = "application/json; charset=utf-8";
    reply.body = body;
    return reply;
}

net::HttpReply json_message(bool ok, const std::string& message, int status = 200) {
    return json_reply(std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" +
                          net::json_escape(message) + "\"}",
                      status);
}

}  // namespace

bool Remote::start(bt::Session& session, uint16_t port, std::string* err) {
    if (server_.running()) return true;
    session_ = &session;
    token_ = make_token();

    if (!server_.start(port, [this](const net::HttpRequest& r) { return handle(r); }, err)) {
        session_ = nullptr;
        return false;
    }
    util::log_line("accès distant : " + url());
    return true;
}

void Remote::stop() {
    if (!server_.running()) return;
    server_.stop();
    session_ = nullptr;
    util::log_line("accès distant arrêté");
}

std::string Remote::url() const {
    const std::string base = server_.url();
    if (base.empty()) return base;
    return base + "?k=" + token_;
}

bool Remote::authorized(const net::HttpRequest& request) const {
    if (token_.empty()) return false;
    if (net::form_field(request.query, "k") == token_) return true;
    return request.header("x-torfoil-key") == token_;
}

net::HttpReply Remote::handle(const net::HttpRequest& request) {
    if (request.path == "/" || request.path == "/index.html") {
        net::HttpReply reply;
        reply.body = remote_page();
        return reply;
    }

    if (!authorized(request)) return json_message(false, "clé absente ou invalide", 403);
    if (!session_) return json_message(false, "moteur indisponible", 503);

    if (request.path == "/api/state" && request.method == "GET") return api_state();
    if (request.path == "/api/add" && request.method == "POST") return api_add(request);
    if (request.path == "/api/action" && request.method == "POST") return api_action(request);

    return json_message(false, "introuvable", 404);
}

net::HttpReply Remote::api_state() const {
    const std::vector<bt::TorrentStatus> torrents = session_->snapshot();

    std::string body = "{\"down\":" + std::to_string(session_->rate_down()) +
                       ",\"up\":" + std::to_string(session_->rate_up()) + ",\"torrents\":[";
    bool first = true;
    for (const bt::TorrentStatus& t : torrents) {
        if (!first) body += ",";
        first = false;
        body += "{\"hash\":\"" + net::json_escape(t.hash_hex) + "\"";
        body += ",\"name\":\"" + net::json_escape(t.name) + "\"";
        body += ",\"state\":\"" + std::string(state_name(t.state)) + "\"";
        body += ",\"size\":" + std::to_string(t.total_size);
        body += ",\"done\":" + std::to_string(t.downloaded);
        body += ",\"up\":" + std::to_string(t.uploaded);
        body += ",\"rate_down\":" + std::to_string(t.rate_down);
        body += ",\"rate_up\":" + std::to_string(t.rate_up);
        body += ",\"peers\":" + std::to_string(t.peers_connected);
        body += ",\"eta\":" + std::to_string(t.eta_s);
        body += ",\"paused\":" + std::string(t.state == bt::TorrentState::Paused ? "true" : "false");
        body += "}";
    }
    body += "]}";
    return json_reply(body);
}

net::HttpReply Remote::api_add(const net::HttpRequest& request) {
    int added = 0;
    std::vector<std::string> refused;

    const std::string type = request.content_type();
    if (type.find("multipart/form-data") != std::string::npos) {
        std::vector<net::MultipartPart> parts;
        if (!net::parse_multipart(type, request.body, parts)) {
            return json_message(false, "formulaire illisible", 400);
        }
        for (const net::MultipartPart& part : parts) {
            if (part.data.empty()) continue;
            // Le fichier passe par la carte : le moteur lit un chemin, pas un
            // tampon, et l'utilisateur garde une trace de ce qui a été envoyé.
            const std::string name = util::sanitize_filename(
                part.filename.empty() ? std::string("recu.torrent") : part.filename);
            const std::string path = "sdmc:/torfoil/inbox/" + name;
            std::FILE* fp = std::fopen(path.c_str(), "wb");
            if (!fp || std::fwrite(part.data.data(), 1, part.data.size(), fp) != part.data.size()) {
                if (fp) std::fclose(fp);
                refused.push_back(name + " : écriture impossible");
                continue;
            }
            std::fclose(fp);
            std::string err;
            if (session_->add_torrent_file(path, &err)) ++added;
            else refused.push_back(name + " : " + (err.empty() ? "refusé" : err));
        }
    } else {
        std::string text = net::form_field(request.body, "magnets");
        if (text.empty()) text = request.body;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            std::string err;
            if (session_->add_magnet(line, &err)) ++added;
            else refused.push_back(err.empty() ? "lien refusé" : err);
        }
    }

    std::string message;
    if (added > 0) message = std::to_string(added) + (added > 1 ? " torrents ajoutés" : " torrent ajouté");
    for (const std::string& r : refused) {
        if (!message.empty()) message += " · ";
        message += r;
    }
    if (message.empty()) message = "rien à ajouter";
    return json_message(added > 0, message);
}

net::HttpReply Remote::api_action(const net::HttpRequest& request) {
    const std::string hash = net::form_field(request.body, "hash");
    const std::string action = net::form_field(request.body, "action");
    if (hash.empty() || action.empty()) return json_message(false, "requête incomplète", 400);
    if (!session_->has_torrent(hash)) return json_message(false, "torrent inconnu", 404);

    if (action == "pause") session_->pause(hash);
    else if (action == "resume") session_->resume(hash);
    else if (action == "skip_check") session_->skip_check(hash);
    else if (action == "remove") session_->remove(hash, false);
    else if (action == "remove_delete") session_->remove(hash, true);
    else return json_message(false, "action inconnue", 400);

    return json_message(true, action);
}

const char* remote_page() {
    return R"HTML(<!doctype html><html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Torfoil</title><style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;padding:20px;font:15px/1.5 system-ui,sans-serif;background:#14161c;color:#e8eaf0}
header{display:flex;align-items:baseline;gap:14px;margin-bottom:18px}
h1{font-size:20px;margin:0}
#rates{color:#8b90a0;font-size:14px}
form{display:flex;gap:8px;margin-bottom:18px;flex-wrap:wrap}
input[type=text]{flex:1 1 320px;background:#1e212a;color:#e8eaf0;border:1px solid #2f3442;
 border-radius:10px;padding:11px;font:inherit}
button{border:0;border-radius:10px;padding:11px 16px;background:#4c7dff;color:#fff;
 font:600 14px system-ui,sans-serif;cursor:pointer}
button.ghost{background:#232733;color:#c9cee0}
button.danger{background:#5a2b2b;color:#ffc9c2}
table{width:100%;border-collapse:collapse}
td,th{padding:10px 8px;border-bottom:1px solid #232733;text-align:left;vertical-align:middle}
th{font-size:12px;text-transform:uppercase;letter-spacing:.04em;color:#8b90a0}
td.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.bar{height:6px;background:#232733;border-radius:3px;overflow:hidden;margin-top:6px}
.bar>i{display:block;height:100%;background:#57c98a}
.name{font-weight:600}
.state{color:#8b90a0;font-size:13px}
#out{margin:14px 0;font-size:14px}
.ok{color:#57c98a}.ko{color:#e5675a}
.empty{color:#8b90a0;padding:40px 0;text-align:center}
</style></head><body>
<header><h1>Torfoil</h1><span id="rates"></span></header>
<form id="add">
<input type="text" name="magnet" placeholder="magnet:?xt=urn:btih:..." autocomplete="off">
<button type="submit">Ajouter</button>
<button type="button" class="ghost" id="pick">Fichier .torrent</button>
<input type="file" id="file" accept=".torrent" multiple hidden>
</form>
<div id="out"></div>
<div id="list"></div>
<script>
const key=new URLSearchParams(location.search).get('k')||localStorage.getItem('torfoil-key')||'';
if(key)localStorage.setItem('torfoil-key',key);
const out=document.getElementById('out');
const say=(m,ok)=>{out.textContent=m;out.className=ok?'ok':'ko'};
const size=n=>{const u=['o','Ko','Mo','Go','To'];let i=0;while(n>=1024&&i<u.length-1){n/=1024;i++}
 return (i?n.toFixed(1):n)+' '+u[i]};
const eta=s=>{if(!s)return '—';if(s<60)return s+' s';if(s<3600)return Math.round(s/60)+' min';
 return Math.round(s/3600)+' h'};
const labels={metadata:'Métadonnées',checking:'Vérification',downloading:'Téléchargement',
 seeding:'Partage',paused:'En pause',queued:'En attente',completed:'Terminé',failed:'Échec'};
async function api(path,opts){const o=opts||{};o.headers=Object.assign(o.headers||{},{'X-Torfoil-Key':key});
 const r=await fetch(path+'?k='+encodeURIComponent(key),o);return r.json()}
async function act(hash,action){
 if(action.startsWith('remove')&&!confirm('Retirer ce torrent ?'))return;
 const j=await api('/api/action',{method:'POST',body:'hash='+hash+'&action='+action});
 say(j.message,j.ok);refresh()}
async function refresh(){
 let s;try{s=await api('/api/state')}catch(e){say('console injoignable',false);return}
 document.getElementById('rates').textContent='↓ '+size(s.down)+'/s · ↑ '+size(s.up)+'/s';
 const list=document.getElementById('list');
 if(!s.torrents.length){list.innerHTML='<p class="empty">Aucun torrent</p>';return}
 let h='<table><tr><th>Nom</th><th class="num">Taille</th><th class="num">↓</th>'+
  '<th class="num">Pairs</th><th class="num">Reste</th><th></th></tr>';
 for(const t of s.torrents){
  const pct=t.size?Math.floor(t.done*100/t.size):0;
  h+='<tr><td><div class="name">'+t.name.replace(/[<>&]/g,'')+'</div>'+
   '<div class="state">'+(labels[t.state]||t.state)+' · '+pct+' %</div>'+
   '<div class="bar"><i style="width:'+pct+'%"></i></div></td>'+
   '<td class="num">'+size(t.size)+'</td>'+
   '<td class="num">'+size(t.rate_down)+'/s</td>'+
   '<td class="num">'+t.peers+'</td>'+
   '<td class="num">'+eta(t.eta)+'</td>'+
   '<td class="num"><button class="ghost" onclick="act(\''+t.hash+'\',\''+
    (t.paused?'resume':'pause')+'\')">'+(t.paused?'Reprendre':'Pause')+'</button> '+
   '<button class="danger" onclick="act(\''+t.hash+'\',\'remove\')">Retirer</button></td></tr>'}
 list.innerHTML=h+'</table>'}
document.getElementById('add').onsubmit=async e=>{e.preventDefault();
 const input=e.target.magnet;if(!input.value.trim())return;
 const j=await api('/api/add',{method:'POST',body:'magnets='+encodeURIComponent(input.value)});
 say(j.message,j.ok);if(j.ok)input.value='';refresh()};
document.getElementById('pick').onclick=()=>document.getElementById('file').click();
document.getElementById('file').onchange=async e=>{
 const fd=new FormData();for(const f of e.target.files)fd.append('files',f,f.name);
 const j=await api('/api/add',{method:'POST',body:fd});say(j.message,j.ok);refresh()};
refresh();setInterval(refresh,2000);
</script></body></html>)HTML";
}

}  // namespace ui

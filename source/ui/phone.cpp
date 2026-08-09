#include "ui/phone.hpp"

#include <sys/stat.h>

#include <cstdio>

#include "util/bytes.hpp"
#include "util/log.hpp"

namespace ui {

namespace {

constexpr uint16_t kPort = 8080;
const char* kInboxDir = "sdmc:/torfoil/inbox";

}  // namespace

bool Phone::set_qr(const std::string& payload) {
    if (util::qr_encode(payload, qr_)) return true;
    // Ne peut se produire qu'au-delà de 106 octets ; un SSID ou une URL de
    // réseau local n'en approche pas. On le traite quand même : un code QR
    // à moitié encodé serait pire qu'absent.
    error_ = "code QR impossible à produire pour « " + payload + " »";
    step_ = Step::Failed;
    return false;
}

bool Phone::start(bt::Session& session, std::string* err) {
    if (step_ != Step::Off && step_ != Step::Failed) return true;
    session_ = &session;
    error_.clear();

    // Le dossier doit exister avant la première arrivée : un .torrent reçu et
    // jeté faute de dossier serait perdu sans que le téléphone en sache rien.
    ::mkdir("sdmc:/torfoil", 0777);
    ::mkdir(kInboxDir, 0777);

    if (!ap_.start(&error_)) {
        step_ = Step::Failed;
        if (err) *err = error_;
        return false;
    }

    if (!server_.start(kPort, [this](const net::HttpRequest& r) { return handle(r); }, &error_)) {
        ap_.stop();
        step_ = Step::Failed;
        if (err) *err = error_;
        return false;
    }
    // Le serveur écoute sur le réseau que la console vient de créer, pas sur
    // celui d'avant.
    server_.set_address(ap_.address());

    if (!set_qr(ap_.wifi_qr_payload())) {
        stop();
        if (err) *err = error_;
        return false;
    }

    step_ = Step::JoinWifi;
    util::log_line("import téléphone : " + ap_.ssid() + " → " + server_.url());
    return true;
}

void Phone::confirm_joined() {
    if (step_ != Step::JoinWifi) return;
    const std::string address = server_.url();
    if (address.empty()) {
        error_ = "adresse du serveur inconnue — le point d'accès n'a pas d'adresse IP";
        step_ = Step::Failed;
        return;
    }
    if (!set_qr(address)) return;
    step_ = Step::OpenPage;
}

void Phone::stop() {
    server_.stop();
    ap_.stop();
    step_ = Step::Off;
    session_ = nullptr;
}

void Phone::note(const std::string& message) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    // Borne haute : personne ne lit deux cents lignes, et la file est vidée à
    // chaque tour de boucle de toute façon.
    if (events_.size() < 32) events_.push_back(message);
}

std::vector<std::string> Phone::take_events() {
    std::lock_guard<std::mutex> lock(events_mutex_);
    std::vector<std::string> out;
    out.swap(events_);
    return out;
}

// La page tient en un fichier, sans script externe : le téléphone est sur le
// réseau de la console, qui n'a plus d'accès à Internet. Tout ce qui pointerait
// vers un CDN afficherait une page nue.
std::string Phone::page() const {
    return R"(<!doctype html><html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Torfoil</title><style>
:root{color-scheme:dark}
body{margin:0;padding:24px;font:16px/1.5 system-ui,sans-serif;background:#14161c;color:#e8eaf0}
h1{font-size:20px;margin:0 0 4px}p.sub{color:#8b90a0;margin:0 0 24px}
label{display:block;font-size:14px;color:#8b90a0;margin:20px 0 6px}
textarea,input[type=file]{width:100%;box-sizing:border-box;background:#1e212a;color:#e8eaf0;
 border:1px solid #2f3442;border-radius:10px;padding:12px;font:inherit}
textarea{height:140px;resize:vertical}
button{width:100%;margin-top:16px;padding:14px;border:0;border-radius:10px;
 background:#4c7dff;color:#fff;font:600 16px system-ui,sans-serif}
#out{margin-top:20px;font-size:14px;white-space:pre-wrap}
.ok{color:#57c98a}.ko{color:#e5675a}
</style></head><body>
<h1>Torfoil</h1><p class="sub">Deposez vos liens magnet ou vos fichiers .torrent.</p>
<form id="f">
<label>Liens magnet, un par ligne</label>
<textarea name="magnets" placeholder="magnet:?xt=urn:btih:..."></textarea>
<label>Fichiers .torrent</label>
<input type="file" name="files" multiple accept=".torrent">
<button type="submit">Envoyer a la console</button>
</form><div id="out"></div>
<script>
const f=document.getElementById('f'),o=document.getElementById('out');
f.onsubmit=async e=>{e.preventDefault();o.textContent='Envoi...';o.className='';
 try{const r=await fetch('/add',{method:'POST',body:new FormData(f)});
  const j=await r.json();o.textContent=j.message;o.className=j.ok?'ok':'ko';
  if(j.ok){f.reset();}}
 catch(err){o.textContent='La console n a pas repondu : '+err;o.className='ko';}};
</script></body></html>)";
}

net::HttpReply Phone::handle(const net::HttpRequest& request) {
    net::HttpReply reply;

    if (request.path == "/" || request.path == "/index.html") {
        reply.body = page();
        return reply;
    }

    if (request.path != "/add" || request.method != "POST") {
        reply.status = 404;
        reply.content_type = "text/plain; charset=utf-8";
        reply.body = "introuvable";
        return reply;
    }

    std::vector<net::MultipartPart> parts;
    if (!net::parse_multipart(request.content_type(), request.body, parts)) {
        reply.status = 400;
        reply.content_type = "application/json";
        reply.body = R"({"ok":false,"message":"formulaire illisible"})";
        return reply;
    }

    int added = 0;
    std::vector<std::string> refused;

    for (const net::MultipartPart& part : parts) {
        if (part.name == "magnets") {
            size_t pos = 0;
            while (pos < part.data.size()) {
                size_t end = part.data.find('\n', pos);
                if (end == std::string::npos) end = part.data.size();
                std::string line = part.data.substr(pos, end - pos);
                pos = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;

                std::string err;
                if (session_ && session_->add_magnet(line, &err)) {
                    ++added;
                    note("Reçu du téléphone : lien magnet");
                } else {
                    refused.push_back(err.empty() ? "lien refusé" : err);
                }
            }
            continue;
        }

        if (part.name != "files" || part.data.empty()) continue;

        // Le fichier est écrit sur la carte avant d'être ajouté : si le moteur
        // le refuse, l'utilisateur garde de quoi comprendre pourquoi, et n'a pas
        // à renvoyer depuis son téléphone.
        const std::string name = util::sanitize_filename(
            part.filename.empty() ? std::string("recu.torrent") : part.filename);
        const std::string path = std::string(kInboxDir) + "/" + name;

        std::FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp || std::fwrite(part.data.data(), 1, part.data.size(), fp) != part.data.size()) {
            if (fp) std::fclose(fp);
            refused.push_back(name + " : écriture sur la carte impossible");
            continue;
        }
        std::fclose(fp);

        std::string err;
        if (session_ && session_->add_torrent_file(path, &err)) {
            ++added;
            note("Reçu du téléphone : " + name);
        } else {
            refused.push_back(name + " : " + (err.empty() ? "refusé" : err));
        }
    }

    imported_.fetch_add(static_cast<uint32_t>(added));

    std::string message;
    if (added > 0) {
        message = std::to_string(added) + (added > 1 ? " torrents ajoutés" : " torrent ajouté");
    }
    for (const std::string& r : refused) {
        if (!message.empty()) message += "\n";
        message += r;
    }
    if (message.empty()) message = "rien à ajouter";

    reply.content_type = "application/json";
    reply.body = std::string("{\"ok\":") + (added > 0 ? "true" : "false") + ",\"message\":\"" +
                 net::json_escape(message) + "\"}";
    return reply;
}

}  // namespace ui

#include "install/installer.hpp"

#include <switch.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "install/archive.hpp"
#include "install/nca.hpp"
#include "util/bytes.hpp"

namespace install {

namespace {

constexpr size_t kCopyChunk = 4 * 1024 * 1024;

std::string hex_of(const uint8_t* data, size_t len) {
    return util::to_hex(data, len);
}

std::string format_rc(const char* what, Result rc) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s (0x%08x)", what, static_cast<unsigned>(rc));
    return buf;
}

Outcome failure(const std::string& message) {
    Outcome r;
    r.ok = false;
    r.message = message;
    return r;
}

const ArchiveEntry* find_entry(const std::vector<ArchiveEntry>& entries, const std::string& name) {
    for (const ArchiveEntry& e : entries) {
        if (util::iequals(e.name, name)) return &e;
    }
    return nullptr;
}

// Recopie un NCA depuis le NSP/XCI vers un emplacement réservé du stockage.
bool write_content(NcmContentStorage& storage, const NcmContentId& content_id, FileReader& file,
                   const ArchiveEntry& entry, const ProgressFn& progress, uint64_t& global_done,
                   uint64_t global_total, const std::string& step, std::string& err) {
    NcmPlaceHolderId placeholder;
    Result rc = ncmContentStorageGeneratePlaceHolderId(&storage, &placeholder);
    if (R_FAILED(rc)) {
        err = format_rc("GeneratePlaceHolderId", rc);
        return false;
    }

    // Un reliquat d'installation interrompue empêcherait la création.
    ncmContentStorageDeletePlaceHolder(&storage, &placeholder);

    rc = ncmContentStorageCreatePlaceHolder(&storage, &content_id, &placeholder,
                                            static_cast<s64>(entry.size));
    if (R_FAILED(rc)) {
        err = format_rc("CreatePlaceHolder", rc);
        return false;
    }

    std::vector<uint8_t> buffer(kCopyChunk);
    uint64_t written = 0;

    while (written < entry.size) {
        const size_t chunk =
            static_cast<size_t>(std::min<uint64_t>(kCopyChunk, entry.size - written));

        if (!file.read(entry.offset + written, buffer.data(), chunk)) {
            ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
            err = "lecture du fichier source impossible (carte SD retirée ?)";
            return false;
        }

        rc = ncmContentStorageWritePlaceHolder(&storage, &placeholder, written, buffer.data(),
                                               chunk);
        if (R_FAILED(rc)) {
            ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
            err = format_rc("WritePlaceHolder", rc);
            return false;
        }

        written += chunk;
        global_done += chunk;

        if (progress) {
            Progress p;
            p.step = step;
            p.done = global_done;
            p.total = global_total;
            if (!progress(p)) {
                ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
                err = "installation annulée";
                return false;
            }
        }
    }

    // Les écritures sont tamponnées par le service : sans ce vidage, un NCA peut
    // être enregistré alors que ses derniers octets ne sont pas encore sur la
    // carte. Le jeu s'installe « avec succès » puis refuse de démarrer.
    ncmContentStorageFlushPlaceHolder(&storage);

    // Un contenu déjà présent bloquerait l'enregistrement.
    bool exists = false;
    if (R_SUCCEEDED(ncmContentStorageHas(&storage, &exists, &content_id)) && exists) {
        ncmContentStorageDelete(&storage, &content_id);
    }

    rc = ncmContentStorageRegister(&storage, &content_id, &placeholder);
    if (R_FAILED(rc)) {
        ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
        err = format_rc("Register", rc);
        return false;
    }

    return true;
}

// PushApplicationRecord n'est pas exposé par libnx (commande jugée sensible) :
// on la dispatche à la main sur IApplicationManagerInterface.
Result push_application_record(uint64_t application_id,
                               const std::vector<NsApplicationContentMetaStatus>& records) {
    Service manager;
    Result rc = nsGetApplicationManagerInterface(&manager);
    if (R_FAILED(rc)) return rc;

    struct {
        u8 last_modified_event;
        u8 padding[7];
        u64 application_id;
    } input = {0x3, {0, 0, 0, 0, 0, 0, 0}, application_id};

    rc = serviceDispatchIn(
        &manager, 16, input,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{records.data(), records.size() * sizeof(NsApplicationContentMetaStatus)}});

    serviceClose(&manager);
    return rc;
}

// DeleteApplicationRecord : indispensable pour réinstaller par-dessus une
// version déjà enregistrée. L'échec n'est pas bloquant (rien à supprimer).
void delete_application_record(uint64_t application_id) {
    Service manager;
    if (R_FAILED(nsGetApplicationManagerInterface(&manager))) return;
    serviceDispatchIn(&manager, 27, application_id);
    serviceClose(&manager);
}

// Import de ticket via le service « es », lui aussi absent de libnx.
Result import_ticket(const std::vector<uint8_t>& ticket, const std::vector<uint8_t>& cert) {
    Service es;
    Result rc = smGetService(&es, "es");
    if (R_FAILED(rc)) return rc;

    rc = serviceDispatch(&es, 1,
                         .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                                          SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                         .buffers = {{ticket.data(), ticket.size()},
                                     {cert.data(), cert.size()}});

    serviceClose(&es);
    return rc;
}

bool read_entry(FileReader& file, const ArchiveEntry& entry, std::vector<uint8_t>& out) {
    if (entry.size == 0 || entry.size > 4 * 1024 * 1024) return false;
    out.resize(static_cast<size_t>(entry.size));
    return file.read(entry.offset, out.data(), out.size());
}

}  // namespace

bool keys_available(std::string* err) {
    KeySet keys;
    return keys.load(err);
}

Outcome install_package(const std::string& path, Target target,
                                  const ProgressFn& progress) {
    std::string err;

    KeySet keys;
    if (!keys.load(&err)) return failure(err);

    FileReader file;
    if (!file.open(path)) return failure("fichier introuvable : " + path);

    std::vector<ArchiveEntry> entries;
    if (!list_installable_contents(file, entries, &err)) return failure(err);

    // --- CNMT ---
    const ArchiveEntry* meta_entry = nullptr;
    for (const ArchiveEntry& e : entries) {
        if (e.name.size() > 9 && e.name.compare(e.name.size() - 9, 9, ".cnmt.nca") == 0) {
            meta_entry = &e;
            break;
        }
    }
    if (!meta_entry) return failure("aucun NCA meta (.cnmt.nca) dans le paquet");

    NcaInfo meta_nca;
    if (!nca_read_header(file, meta_entry->offset, keys, meta_nca, &err)) return failure(err);

    std::vector<uint8_t> cnmt_blob;
    if (!nca_extract_cnmt(file, meta_entry->offset, meta_nca, cnmt_blob, &err)) {
        return failure(err);
    }

    CnmtInfo cnmt;
    if (!parse_cnmt(cnmt_blob, cnmt, &err)) return failure(err);

    // --- ouverture du stockage ---
    const NcmStorageId storage_id =
        target == Target::Sd ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    NcmContentStorage storage;
    Result rc = ncmOpenContentStorage(&storage, storage_id);
    if (R_FAILED(rc)) return failure(format_rc("ncmOpenContentStorage", rc));

    NcmContentMetaDatabase database;
    rc = ncmOpenContentMetaDatabase(&database, storage_id);
    if (R_FAILED(rc)) {
        ncmContentStorageClose(&storage);
        return failure(format_rc("ncmOpenContentMetaDatabase", rc));
    }

    // --- place disponible ---
    uint64_t total_bytes = meta_entry->size;
    for (const ContentRecord& record : cnmt.contents) total_bytes += record.size;

    s64 free_space = 0;
    if (R_SUCCEEDED(ncmContentStorageGetFreeSpaceSize(&storage, &free_space)) &&
        static_cast<uint64_t>(free_space) < total_bytes) {
        ncmContentMetaDatabaseClose(&database);
        ncmContentStorageClose(&storage);
        return failure("espace insuffisant : " + util::human_size(total_bytes) + " requis, " +
                       util::human_size(static_cast<uint64_t>(free_space)) + " disponibles");
    }

    // --- recopie des contenus ---
    uint64_t done = 0;
    std::vector<NcmContentInfo> infos;
    infos.reserve(cnmt.contents.size() + 1);

    auto cleanup = [&]() {
        ncmContentMetaDatabaseClose(&database);
        ncmContentStorageClose(&storage);
    };

    for (const ContentRecord& record : cnmt.contents) {
        const std::string name = hex_of(record.nca_id, 16) + ".nca";
        const ArchiveEntry* entry = find_entry(entries, name);
        if (!entry) {
            cleanup();
            return failure("contenu manquant dans le paquet : " + name);
        }

        // Le CNMT annonce une taille, le paquet en contient une autre : le NSP
        // est tronqué ou mal reconstruit. Installer quand même produirait un
        // titre qui s'installe puis plante au lancement.
        if (entry->size != record.size) {
            cleanup();
            return failure("taille incohérente pour " + name + " : " +
                           util::human_size(entry->size) + " dans le paquet, " +
                           util::human_size(record.size) + " annoncés — paquet incomplet");
        }

        NcmContentId content_id;
        std::memcpy(content_id.c, record.nca_id, 16);

        if (!write_content(storage, content_id, file, *entry, progress, done, total_bytes,
                           "Installation du contenu", err)) {
            cleanup();
            return failure(err);
        }

        NcmContentInfo info{};
        info.content_id = content_id;
        info.size_low = static_cast<u32>(record.size & 0xffffffffu);
        info.size_high = static_cast<u8>((record.size >> 32) & 0xff);
        info.attr = 0;
        info.content_type = record.type;
        info.id_offset = record.id_offset;
        infos.push_back(info);
    }

    // Le NCA meta lui-même n'est pas listé dans le CNMT : on l'ajoute.
    {
        NcmContentId meta_id;
        // Le nom du fichier porte l'identifiant du contenu.
        const std::string stem = meta_entry->name.substr(0, meta_entry->name.size() - 9);
        if (stem.size() != 32 || !util::from_hex(stem, meta_id.c, 16)) {
            cleanup();
            return failure("nom du NCA meta inattendu : " + meta_entry->name);
        }

        if (!write_content(storage, meta_id, file, *meta_entry, progress, done, total_bytes,
                           "Installation des métadonnées", err)) {
            cleanup();
            return failure(err);
        }

        NcmContentInfo info{};
        info.content_id = meta_id;
        info.size_low = static_cast<u32>(meta_entry->size & 0xffffffffu);
        info.size_high = static_cast<u8>((meta_entry->size >> 32) & 0xff);
        info.attr = 0;
        info.content_type = NcmContentType_Meta;
        info.id_offset = 0;
        infos.push_back(info);
    }

    // --- entrée de métadonnées ---
    std::vector<uint8_t> meta_buffer;
    {
        NcmContentMetaHeader header{};
        header.extended_header_size = static_cast<u16>(cnmt.extended_header.size());
        header.content_count = static_cast<u16>(infos.size());
        header.content_meta_count = cnmt.content_meta_count;
        header.attributes = cnmt.attributes;
        header.storage_id = 0;

        const uint8_t* header_bytes = reinterpret_cast<const uint8_t*>(&header);
        meta_buffer.insert(meta_buffer.end(), header_bytes, header_bytes + sizeof(header));
        meta_buffer.insert(meta_buffer.end(), cnmt.extended_header.begin(),
                           cnmt.extended_header.end());

        const uint8_t* infos_bytes = reinterpret_cast<const uint8_t*>(infos.data());
        meta_buffer.insert(meta_buffer.end(), infos_bytes,
                           infos_bytes + infos.size() * sizeof(NcmContentInfo));
        meta_buffer.insert(meta_buffer.end(), cnmt.content_meta_infos.begin(),
                           cnmt.content_meta_infos.end());
    }

    NcmContentMetaKey key{};
    key.id = cnmt.title_id;
    key.version = cnmt.version;
    key.type = cnmt.meta_type;
    key.install_type = NcmContentInstallType_Full;

    rc = ncmContentMetaDatabaseSet(&database, &key, meta_buffer.data(), meta_buffer.size());
    if (R_FAILED(rc)) {
        cleanup();
        return failure(format_rc("ContentMetaDatabaseSet", rc));
    }

    rc = ncmContentMetaDatabaseCommit(&database);
    if (R_FAILED(rc)) {
        cleanup();
        return failure(format_rc("ContentMetaDatabaseCommit", rc));
    }

    // --- ticket éventuel ---
    for (const ArchiveEntry& entry : entries) {
        if (entry.name.size() < 4) continue;
        if (entry.name.compare(entry.name.size() - 4, 4, ".tik") != 0) continue;

        const std::string cert_name = entry.name.substr(0, entry.name.size() - 4) + ".cert";
        const ArchiveEntry* cert_entry = find_entry(entries, cert_name);
        if (!cert_entry) continue;

        std::vector<uint8_t> ticket;
        std::vector<uint8_t> cert;
        if (!read_entry(file, entry, ticket) || !read_entry(file, *cert_entry, cert)) continue;

        import_ticket(ticket, cert);  // échec non bloquant : titre sans DRM
    }

    // --- enregistrement applicatif (tuile sur le menu HOME) ---
    const uint64_t app_id = cnmt.application_id();

    std::vector<NsApplicationContentMetaStatus> records(1);
    records[0].meta_type = cnmt.meta_type;
    records[0].storageID = static_cast<u8>(storage_id);
    records[0].rights_check = 0;
    records[0].reserved = 0;
    records[0].version = cnmt.version;
    records[0].application_id = cnmt.title_id;

    delete_application_record(app_id);
    rc = push_application_record(app_id, records);

    cleanup();

    if (R_FAILED(rc)) {
        return failure(format_rc("PushApplicationRecord", rc) +
                       " — contenu installé mais absent du menu HOME");
    }

    Outcome result;
    result.ok = true;
    result.title_id = app_id;
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(app_id));
    result.title_id_hex = hex;
    result.message = "Installé — " + util::human_size(total_bytes);
    return result;
}

}  // namespace install

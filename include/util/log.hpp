// Journal de diagnostic écrit sur la carte SD.
//
// Sur une console il n'y a ni terminal ni débogueur : quand quelque chose se
// passe mal, la seule trace exploitable est un fichier. Ce journal enregistre
// ce qui a démarré, ce qui a échoué et pourquoi — c'est ce qu'on demande à
// l'utilisateur d'envoyer quand un problème n'est pas reproductible ailleurs.
#pragma once

#include <string>

namespace util {

// Ouvre (et fait tourner) sdmc:/torfoil/torfoil.log.
void log_open(const std::string& path);
void log_close();

void log_line(const std::string& message);

// Variante formatée, pour les codes d'erreur.
void log_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace util

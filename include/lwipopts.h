// Configuration de lwIP.
//
// Mode NO_SYS : pas de threads internes, pas d'API sockets. Tout est piloté
// depuis un seul fil d'exécution via l'API brute (callbacks), sous un verrou
// tenu par le transport. C'est ce qui évite d'avoir à porter un sys_arch complet
// vers Horizon OS.
#pragma once

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

// --- protocoles ---
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_IGMP 0
#define LWIP_AUTOIP 0
#define LWIP_DHCP 0

// Le tunnel est une liaison point à point : pas d'Ethernet, pas d'ARP.
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_NETIF_HOSTNAME 0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0

// --- mémoire ---
// Ces valeurs décident combien de pairs peuvent réellement dialoguer quand le
// VPN est actif : tout le trafic passe alors par cette pile, et ses tampons sont
// partagés entre toutes les connexions. Trop serrées, on trouve trois cents
// pairs et on n'en sert que trois — le plafond n'est pas le réseau, c'est ici.
//
// Un client BitTorrent veut beaucoup de connexions de débit modeste. On donne
// donc largement en nombre de tampons, et on reste sobre par connexion : une
// fenêtre de 16 Ko suffit, c'est la taille d'un bloc.
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_ALIGNMENT 8
#define MEM_SIZE (8 * 1024 * 1024)

#define MEMP_NUM_PBUF 256
#define MEMP_NUM_UDP_PCB 16
#define MEMP_NUM_TCP_PCB 96
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 1024
#define MEMP_NUM_SYS_TIMEOUT 32

// ~3 Mo de tampons de réception. C'est le poste le plus coûteux, et celui qui
// libère vraiment le nombre de connexions simultanées.
#define PBUF_POOL_SIZE 2048
#define PBUF_POOL_BUFSIZE 1536

// --- TCP ---
// MTU du tunnel : 1420 est la valeur usuelle de WireGuard (1500 - 20 IP - 8 UDP
// - 32 d'en-tête et d'étiquette). Le MSS retire encore 40 octets d'en-têtes
// IP+TCP. Se tromper ici fabrique des paquets fragmentés que beaucoup de pairs
// laissent tomber en silence.
#define TCP_MSS 1380
#define TCP_SND_BUF (16 * TCP_MSS)
#define TCP_SND_QUEUELEN (4 * TCP_SND_BUF / TCP_MSS)
// Fenetre de reception. C'est elle qui fixe le debit d'un pair : au mieux
// TCP_WND / temps d'aller-retour. Les 16 Ko d'origine plafonnaient chaque pair a
// ~160 Ko/s pour 100 ms de latence, et le VPN ajoute justement de la latence.
// 44 x MSS = 60 720 octets, juste sous les 65 535 au-dela desquels il faudrait
// negocier la mise a l'echelle de fenetre.
#define TCP_WND (44 * TCP_MSS)
#define TCP_MAXRTX 8
#define TCP_SYNMAXRTX 4
#define LWIP_TCP_KEEPALIVE 1
#define TCP_QUEUE_OOSEQ 1
#define TCP_LISTEN_BACKLOG 0

// --- DNS ---
#define DNS_TABLE_SIZE 8
#define DNS_MAX_NAME_LENGTH 256
#define DNS_MAX_SERVERS 2

// --- divers ---
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_CHKSUM_ALGORITHM 2
#define LWIP_STATS 0
#define LWIP_DEBUG 0
#define LWIP_TIMEVAL_PRIVATE 0

// Sans ARP ni Ethernet, une interface point à point n'a pas de découverte de
// voisin : on écrit directement dans le tunnel.
#define LWIP_SINGLE_NETIF 1

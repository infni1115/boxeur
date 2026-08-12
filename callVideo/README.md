# callVideo

Application d'appel vidéo à deux, minimaliste :

- **Backend : C pur** (POSIX sockets + pthreads, zéro dépendance externe —
  même le SHA1 et le base64 nécessaires au handshake WebSocket sont
  réimplémentés à la main dans `src/`). Un seul binaire, un seul port.
- **Frontend : HTML + CSS + JS vanilla**, aucun framework. Utilise l'API
  WebRTC native du navigateur (`RTCPeerConnection`, `getUserMedia`).

## Pourquoi c'est léger

Le serveur C ne fait que deux choses :

1. Servir les 3 fichiers statiques (`index.html`, `app.js`, `style.css`).
2. Parler juste assez de WebSocket (RFC 6455) pour relayer entre les deux
   navigateurs d'un même "salon" les messages de signalisation WebRTC
   (offer/answer SDP, candidats ICE).

**Le flux audio/vidéo ne transite jamais par le serveur.** Une fois la
connexion WebRTC établie, les deux navigateurs échangent directement leurs
flux média en pair-à-pair (via STUN pour la traversée NAT). Le serveur ne
voit que quelques kilo-octets de JSON au tout début de l'appel.

## Build

```sh
make
```

Produit `bin/server`. Nécessite juste un compilateur C (gcc/clang) et
pthreads — rien d'autre.

## Lancer

```sh
./bin/server [port] [docroot]
# valeurs par défaut : 8080, ./public
```

Puis ouvrir `http://<ip-du-serveur>:8080/` sur les deux postes, entrer le
même code de salon des deux côtés, et cliquer sur "Rejoindre l'appel".

> Note réseau : le premier arrivé dans un salon devient automatiquement
> l'initiateur WebRTC dès que le second rejoint — aucune configuration
> manuelle n'est nécessaire. Le projet utilise un serveur STUN public
> (`stun.l.google.com`) pour la traversée NAT ; ça suffit dans l'immense
> majorité des cas (même réseau, box grand public...). Sur un réseau
> d'entreprise avec NAT symétrique strict, un serveur TURN serait
> nécessaire (non inclus, hors scope pour rester léger).

## Structure

```
src/
  server.c   - HTTP statique + handshake WebSocket + relais de salons
  sha1.c/h   - SHA1 minimal (handshake WebSocket uniquement)
  base64.c/h - Base64 minimal (handshake WebSocket uniquement)
public/
  index.html - écran de connexion + écran d'appel
  app.js     - logique WebRTC (RTCPeerConnection, signalisation via WS)
  style.css  - thème sombre, vidéo distante plein écran + PiP locale
```

## Sécurité / limites connues

- Pas de TLS natif (WS en clair). Pour du HTTPS/WSS, mettre un reverse
  proxy (nginx/caddy) devant, ou lancer derrière un tunnel.
- Salons non protégés par mot de passe : toute personne connaissant le
  code peut rejoindre (max 2 personnes par salon, la 3e est rejetée).
- Pas de TURN : échoue si les deux réseaux sont derrière un NAT symétrique
  strict des deux côtés.

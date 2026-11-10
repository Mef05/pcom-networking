# Router Dataplane - Tema 1 PCOM

## Subiect 1 - Procesul de dirijare (Forwarding)

La primirea unui pachet IPv4, routerul executa urmatorii pasi in ordine:

1. **Verificare checksum IP** - se recalculeaza checksum-ul si se compara cu cel din header. Pachetele corupte sunt aruncate silentios.
2. **Verificare destinatie proprie** - se verifica daca IP-ul destinatie apartine uneia dintre interfetele routerului. Daca da, pachetul este tratat local (vezi Subiect 4 - ICMP Echo). IP-urile proprii sunt retinute intr-un `unordered_set` populat la startup, pentru O(1) per pachet.
3. **Cautare ruta (LPM)** - se cauta cea mai lunga potrivire de prefix in trie (vezi Subiect 2). Daca nu exista ruta, se trimite ICMP Destination Unreachable.
4. **Verificare si decrementare TTL** - daca TTL <= 1, se trimite ICMP Time Exceeded si pachetul e aruncat. Altfel, TTL e decrementat.
5. **Recalculare checksum IP** - dupa modificarea TTL, checksum-ul IP este recalculat.
6. **Rescriere adrese L2 si forwarding** - se cauta MAC-ul next-hop in cache-ul ARP. Daca exista, se rescriu adresele Ethernet si pachetul este trimis pe interfata corecta. Daca nu exista, pachetul e pus in coada si se trimite un ARP Request (vezi Subiect 3).

In `subject1.png`: h-0 (192.168.0.2) trimite `ping -c 4 192.168.1.2` catre h-1. In Wireshark pe interfata `r-1` a router-0 se observa pachetele ICMP Echo Request si Echo Reply forwarded, cu TTL decrementat fata de cel original si adresele MAC L2 rescrise (sursa = MAC interfata `r-1` a routerului, destinatie = MAC h-1).

Screenshot: `subject1.png`

---

## Subiect 2 - Longest Prefix Match eficient (Trie)

Implementarea LPM foloseste un **binary trie** in locul cautarii liniare.

La initializare, toate rutele din `rtable.txt` sunt inserate in trie. Pentru fiecare ruta, se determina numarul de biti din masca (lungimea prefixului) si se parcurge trie-ul bit cu bit (MSB first) pe adancimea corespunzatoare, creand noduri dupa nevoie. La fiecare nod terminal se stocheaza un pointer la intrarea din tabela de rutare.

La cautarea unui IP destinatie, se parcurge trie-ul urmand bitii IP-ului (MSB first) si se retine cel mai recent nod cu un `entry != NULL`. Aceasta da automat cea mai lunga potrivire de prefix.

**Complexitate:** O(32) per cautare (fix, independent de numarul de rute), fata de O(n) la cautarea liniara.

---

## Subiect 3 - Protocolul ARP

Implementarea ARP acopera doua directii:

**Raspuns la ARP Request:**
La primirea unui ARP Request cu `target IP` egal cu unul dintre IP-urile proprii ale routerului, routerul construieste un ARP Reply direct pe bufferul primit: inverseaza MAC-urile Ethernet, seteaza `opcode = 2`, copiaza adresele sender/target si completeaza `shwa` cu MAC-ul propriu al interfetei.

**Trimitere ARP Request si procesare ARP Reply:**
Cand un pachet IPv4 trebuie forwardat dar MAC-ul next-hop nu se afla in cache, pachetul este salvat intr-o `std::list<PendingPacket>` si se trimite un ARP Request broadcast pe interfata corecta. La primirea unui ARP Reply, MAC-ul sender-ului este adaugat in `arp_table` (un `unordered_map<uint32_t, array<uint8_t,6>>`), apoi lista de pachete in asteptare este parcursa si toate pachetele cu `next_hop_ip` egal cu IP-ul din reply sunt completate cu MAC-ul invatat si trimise. Folosind `std::list`, stergerea elementelor din lista se face in O(1) prin `erase(iterator)`, fara a mai fi nevoie de o coada temporara.

In `subject3.png`: dupa un restart al router-0 (cache ARP gol), h-0 trimite `ping -c 1 192.168.1.2`. In Wireshark pe interfata `r-1` se observa secventa completa: ARP Request broadcast (`who has 192.168.1.2? Tell 192.168.1.1`) trimis de router, ARP Reply de la h-1 (`192.168.1.2 is at de:ad:be:ef:00:01`), urmat de ICMP Echo Request forwardat si ICMP Echo Reply intors.

Screenshot: `subject3.png`

---

## Subiect 4 - Protocolul ICMP

Routerul genereaza sau raspunde la mesaje ICMP in trei situatii:

**ICMP Echo Reply (type 0):**
Daca pachetul este destinat routerului si este un ICMP Echo Request (type 8, code 0), routerul modifica pachetul direct in buffer: inverseaza MAC-urile si adresele IP, recalculeaza checksum-urile IP si ICMP si retrimite pachetul pe aceeasi interfata. Datele ICMP (id, seq, payload) sunt pastrate intacte.

**ICMP Time Exceeded (type 11, code 0):**
Generat cand un pachet IPv4 are TTL <= 1. Pachetul ICMP contine ca payload header-ul IP original plus primii 8 bytes ai payload-ului original (conform RFC 792).

**ICMP Destination Unreachable (type 3, code 0):**
Generat cand nu exista ruta pentru IP-ul destinatie in tabela de rutare.

In ambele cazuri de eroare, pachetul ICMP este construit intr-un buffer separat: header Ethernet nou (MAC sursa = interfata routerului, MAC destinatie = MAC sursa original), header IP nou (sursa = IP interfata routerului, destinatie = IP sursa original, TTL = 64), header ICMP cu checksum calculat peste ICMP header + payload.

In `subject4.png`: doua scenarii rulate din h-0, capturate in Wireshark pe interfata `r-0`. Primul: `ping -c 3 192.168.0.1` — Wireshark arata ICMP type 8 (Echo Request destinat routerului) si type 0 (Echo Reply cu sursa 192.168.0.1, datele ICMP id/seq/payload pastrate intacte). Al doilea: `ping -c 2 -t 1 192.168.1.2` — Wireshark arata ICMP type 11 code 0 (Time Exceeded) trimis de router catre 192.168.0.2, cu payload continand header-ul IP original al pachetului dropped plus primii 8 bytes de deasupra lui.

Screenshot: `subject4.png`

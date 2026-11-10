# Walkthrough Protocol POLI-TCP

Fluxul datelor si starea protocolului urmaresc urmatoarea serie de pasi:

## 1. Stabilirea Conexiunii (Handshake)
1. Aplicatia apeleaza `setup_connection`. Se asambleaza un pachet `TYPE_SYN` care pleaca spre portul global al serverului.
2. Pe server, `wait4connect` preia SYN-ul. Se deschide un socket pe un port efemer nou si se raspunde cu `TYPE_SYN_ACK` care contine noul port util.
3. Clientul actualizeaza structura conexiunii cu acest port dedicat si trimite inapoi pachetul final `TYPE_ACK`.

## 2. Transmisia Datelor (Sender)
1. Aplicatia apeleaza `send_data` cu buffer-ul ei de continut.
2. Daca `next_seq_num` depaseste limita impusa de fereastra glisanta, se apeleaza `pthread_cond_wait`, cedand controlul CPU-ului pana e trezit de sosirea unui ACK valid.
3. Altfel, functia fragmenteaza datele si le incarca asamblate direct in `send_ring` (un vector circular static, pentru evitarea alocarilor dinamice de heap).
4. Pe fiecare pachet stocat se retine un timestamp absolut luand ceasul sistemului (`clock_gettime`).
5. Pentru a mari performanta, apelul blocant de kernel `sendto` se executa explicit cu mutex-ul scos, neafectand celelalte procese concurente din handler.

## 3. Receptia Datelor (Receiver)
1. Procesul asincron `receiver_handler` extrage datagrame sosite in buffer-ul de retea.
2. Continutul util din datagrama este depus direct in slotul corect calculat prin modulo in `recv_ring`. 
3. Handler-ul impinge cursorul intern `expected_seq` inainte ocolind spatiile libere, apoi impacheteaza si returneaza un pachet cumulative `TYPE_ACK`. Trimite semnal conditionat daca functia aplicatiei asteapta date.
4. Dinspre aplicatie se apeleaza `recv_data`. Functia blocheaza daca structura in-order e goala. Altfel, goleste `recv_ring`-ul liniar si returneaza cantitatea consumata in bufferul user-ului.

## 4. Gestiunea ACK-urilor si Timeout-urilor (Selective Repeat)
1. In background la client, `sender_handler` se blocheaza intr-un poll. La declansarea primirii de pachete citeste succesiv pe `MSG_DONTWAIT` tot bulk-ul disponibil de ACK-uri sub acelasi lock.
2. Avanseaza cursorul de ferestre recunoscute, deblocheaza pointerul `send_data` daca astepta spatiu, si curata slot-urile trecute in ring buffer.
3. Tot la primirea unui ACK corect inainteaza estimarile pentru calculul variabilelor SRTT si RTO.
4. Daca poll-ul din client intoarce eveniment ca timerfd-ul general a expirat (timeout), functia face un sweep secvential prin ring buffer-ul slot-urilor si re-emite doar pachetele care la timestamp individual au o varsta care trece pragul lui RTO.

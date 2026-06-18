// ============================================================
//  ANTRENARE MODUL VOCAL - ELECHOUSE Voice Recognition V3
//  ESP32 Arduino Core 3.x - FARA BIBLIOTECA EXTERNA
//
//  Modulul trimite frame-uri cu payload TEXT ASCII:
//    "Speak now"  = vorbeste acum (CMD=0x0A in frame)
//    "No voice"   = nu s-a auzit nimic, incearca din nou
//    "Trained"    = succes antrenare
//
//  FLUX CORECT train:
//    TX: AA 03 20 00 0A
//    RX: "Speak now"  -> VORBESTI
//    RX: "No voice"   -> n-ai vorbit la timp, modulul reincerca automat
//      sau "Speak now" -> vorbesti din nou
//    ...repeta pana primesti "Trained" sau depasesti retry-urile
// ============================================================

#define RECORD_CAM1_ON    0
#define RECORD_CAM1_OFF   1
#define RECORD_CAM2_ON    2
#define RECORD_CAM2_OFF   3

#define VR_TIMEOUT_RAPID   3000
#define VR_TIMEOUT_TRAIN  15000   // 15s - timp generos pentru vorbire

#define BUF_MAX 64
uint8_t vrBuf[BUF_MAX];
int     vrBufLen = 0;

String inputSerial = "";

// ============================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  delay(500);
  while (Serial2.available()) Serial2.read();
  Serial.println("\n================================================");
  Serial.println("  ANTRENARE Voice Recognition V3");
  Serial.println("================================================");
  afiseazaMeniu();
  Serial.print("\n> ");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      inputSerial.trim();
      if (inputSerial.length() > 0) {
        Serial.println();
        proceseazaComanda(inputSerial);
        inputSerial = "";
        Serial.print("\n> ");
      }
    } else {
      inputSerial += c;
      Serial.print(c);
    }
  }
}

// ============================================================
//  TRIMITE FRAME BINAR
//  LEN = 1(LEN) + 1(CMD) + dataLen
// ============================================================
void vrSend(uint8_t cmd, uint8_t* data, int dataLen) {
  uint8_t len = 1 + 1 + dataLen;
  Serial2.write((uint8_t)0xAA);
  delayMicroseconds(800);
  Serial2.write(len);
  delayMicroseconds(800);
  Serial2.write(cmd);
  for (int i = 0; i < dataLen; i++) {
    delayMicroseconds(800);
    Serial2.write(data[i]);
  }
  delayMicroseconds(800);
  Serial2.write((uint8_t)0x0A);
  Serial2.flush();
}

// ============================================================
//  CITESTE UN FRAME COMPLET BAZAT PE LEN
//  Frame: AA | LEN | CMD+DATA (LEN-1 bytes) | 0A
//  Returneaza vrBufLen sau -1 la timeout
// ============================================================
int vrCitesteFrame(unsigned long timeoutMs) {
  unsigned long start = millis();
  memset(vrBuf, 0, BUF_MAX);
  vrBufLen = 0;

  // Asteapta AA
  while (millis() - start < timeoutMs) {
    if (Serial2.available()) {
      uint8_t b = Serial2.read();
      if (b == 0xAA) { vrBuf[0] = 0xAA; break; }
    }
    delay(1);
  }
  if (millis() - start >= timeoutMs) return -1;

  // Citeste LEN
  while (millis() - start < timeoutMs) {
    if (Serial2.available()) { vrBuf[1] = Serial2.read(); break; }
    delay(1);
  }
  if (millis() - start >= timeoutMs) return -1;

  uint8_t len = vrBuf[1];
  if (len < 2 || len > BUF_MAX - 3) {
    Serial.printf("[FRAME ERR] LEN=%d invalid\n", len);
    return -1;
  }

  // Citeste LEN-1 bytes (CMD + DATA)
  int toRead = len - 1;
  int idx = 2;
  while (toRead > 0 && millis() - start < timeoutMs) {
    if (Serial2.available()) { vrBuf[idx++] = Serial2.read(); toRead--; }
    delay(1);
  }
  if (toRead > 0) return -1;

  // Citeste 0x0A final
  while (millis() - start < timeoutMs) {
    if (Serial2.available()) { vrBuf[idx++] = Serial2.read(); break; }
    delay(1);
  }
  if (millis() - start >= timeoutMs) return -1;

  vrBufLen = idx;
  return vrBufLen;
}

// ============================================================
//  EXTRAGE TEXTUL ASCII DIN PAYLOAD
//  Frame: AA LEN 0A 00 TEXT... 0D 0A
//  Textul e la vrBuf[4] pana la primul 0x0D sau 0x0A
// ============================================================
String extragText() {
  String text = "";
  // Payload incepe la index 4 (dupa AA LEN CMD 00)
  for (int i = 4; i < vrBufLen; i++) {
    uint8_t b = vrBuf[i];
    if (b == 0x0D || b == 0x0A) break;
    if (b >= 0x20 && b <= 0x7E) text += (char)b;
  }
  return text;
}

// ============================================================
//  AFISEAZA FRAME IN HEX + TEXT DECODAT
// ============================================================
void afiseazaFrame(const char* prefix, int len) {
  Serial.print(prefix);
  if (len <= 0) { Serial.println(" [TIMEOUT]"); return; }
  for (int i = 0; i < len; i++) Serial.printf(" %02X", vrBuf[i]);
  String text = extragText();
  if (text.length() > 0) Serial.printf("  -> \"%s\"", text.c_str());
  Serial.println();
}

// ============================================================
//  ANTRENEAZA UN RECORD
//
//  Modulul trimite "Speak now" de mai multe ori daca nu aude voce.
//  Asteptam pana primim "Trained" sau depasim MAX_RETRY incercari.
//  La fiecare "Speak now" afisam mesajul si asteptam vocea.
//  La "No voice" afisam avertisment si asteptam urmatorul prompt.
// ============================================================
void antreneazaRecord(int nr) {
  Serial.println("\n----------------------------------------");
  Serial.printf("ANTRENARE Record %d", nr);
  switch (nr) {
    case RECORD_CAM1_ON:  Serial.print(" [Camera 1 - PORNIRE]"); break;
    case RECORD_CAM1_OFF: Serial.print(" [Camera 1 - OPRIRE]");  break;
    case RECORD_CAM2_ON:  Serial.print(" [Camera 2 - PORNIRE]"); break;
    case RECORD_CAM2_OFF: Serial.print(" [Camera 2 - OPRIRE]");  break;
    default:              Serial.printf(" [Record %d]", nr);     break;
  }
  Serial.println("\n----------------------------------------");
  Serial.println("Asculta mesajele si vorbeste cand apare '>>> VORBESTE ACUM! <<<'");
  Serial.println("Ai ~3 secunde sa vorbesti dupa fiecare prompt!");
  Serial.println();

  while (Serial2.available()) Serial2.read();
  delay(100);

  // Trimite comanda train
  uint8_t rec = (uint8_t)nr;
  Serial.printf("[TX] AA 03 20 %02X 0A\n", rec);
  vrSend(0x20, &rec, 1);

  // Asteapta raspunsuri - pana la 10 frame-uri (modulul poate repeta)
  int speakCount = 0;
  bool antrenat  = false;
  int  maxFrames = 10;

  for (int f = 0; f < maxFrames; f++) {
    int ret = vrCitesteFrame(VR_TIMEOUT_TRAIN);

    if (ret < 0) {
      Serial.println("[EROARE] Timeout! Modulul nu mai raspunde.");
      break;
    }

    afiseazaFrame("[RX]", ret);
    String text = extragText();
    text.toLowerCase();

    if (text.indexOf("speak") >= 0) {
      // ---- MODULUL ASCULTA ----
      speakCount++;
      if (speakCount == 1) {
        Serial.println("\n>>> VORBESTE ACUM! (prima inregistrare) <<<");
      } else {
        Serial.printf("\n>>> VORBESTE DIN NOU! (incercarea %d) <<<\n", speakCount);
      }
      Serial.println("    [Ai ~3 secunde - vorbeste imediat!]");

    } else if (text.indexOf("no voice") >= 0 || text.indexOf("novoice") >= 0) {
      // ---- NU S-A AUZIT NIMIC ----
      Serial.println("\n[!] Nu s-a detectat voce! Vorbeste mai tare si mai aproape.");
      Serial.println("    Modulul va incerca din nou automat...");

    } else if (text.indexOf("train") >= 0 || text.indexOf("success") >= 0) {
      // ---- SUCCES ----
      antrenat = true;
      Serial.printf("\n[SUCCES] Record %d antrenat cu succes!\n", nr);
      break;

    } else if (vrBuf[2] == 0x20) {
      // ---- RASPUNS BINAR CMD 0x20 = rezultat final ----
      uint8_t n   = vrBuf[3];
      uint8_t sta = (vrBufLen >= 7) ? vrBuf[5] : vrBuf[4];
      Serial.printf("[RESULT BINAR] N=%d STA=0x%02X\n", n, sta);
      if (n > 0 || sta == 0x00) {
        antrenat = true;
        Serial.printf("\n[SUCCES] Record %d antrenat!\n", nr);
      } else {
        Serial.printf("\n[ESEC] STA=0x%02X\n", sta);
      }
      break;

    } else if (text.length() > 0) {
      // Alt mesaj text - afiseaza si continua
      Serial.printf("[INFO] Mesaj modul: \"%s\"\n", text.c_str());
    }
  }

  if (!antrenat) {
    Serial.println("\n[NEFINALIZAT] Verifica LED-ul modulului:");
    Serial.println("  - Verde clipeste = antrenat cu succes");
    Serial.println("  - Rosu = esec, reincearca: train " + String(nr));
    Serial.println("Sau scrie 'check' pentru a verifica statusul.");
  }
}

// ============================================================
//  INCARCA RECORDURI IN RECUNOSCATOR
// ============================================================
void incarcaRecorduri() {
  Serial.println("\n[LOAD] Incarc recordurile 0,1,2,3...");
  Serial.println("[TX] AA 06 30 00 01 02 03 0A");
  while (Serial2.available()) Serial2.read();

  uint8_t recs[] = {0x00, 0x01, 0x02, 0x03};
  vrSend(0x30, recs, 4);

  int ret = vrCitesteFrame(VR_TIMEOUT_RAPID);
  if (ret < 0) { Serial.println("[EROARE] Timeout!"); return; }
  afiseazaFrame("[RX]", ret);

  if (vrBuf[2] == 0x30) {
    uint8_t nrLoaded = vrBuf[3];
    Serial.printf("[OK] %d recorduri incarcate! Modulul asculta activ.\n", nrLoaded);
  } else {
    String text = extragText();
    if (text.length() > 0) Serial.printf("[INFO] Raspuns: \"%s\"\n", text.c_str());
    else Serial.println("[INFO] Comanda trimisa. Verifica cu 'vcheck'.");
  }
}

// ============================================================
//  VERIFICA RECORDURI ANTRENATE
// ============================================================
void verificaRecorduri() {
  Serial.println("\n[CHECK] Verific recordurile 0-3...\n");
  while (Serial2.available()) Serial2.read();

  uint8_t data[] = {0x00, 0x01, 0x02, 0x03};
  vrSend(0x02, data, 4);

  int ret = vrCitesteFrame(VR_TIMEOUT_RAPID);
  if (ret < 0) { Serial.println("[EROARE] Timeout!"); return; }
  afiseazaFrame("[RX]", ret);

  if (vrBuf[2] == 0x02) {
    uint8_t nTotal = vrBuf[3];
    Serial.printf("Recorduri antrenate: %d/4\n\n", nTotal);
    int i = 4;
    while (i + 1 < vrBufLen - 1) {
      uint8_t r   = vrBuf[i];
      uint8_t sta = vrBuf[i+1];
      Serial.printf("  Record %d", r);
      switch (r) {
        case RECORD_CAM1_ON:  Serial.print(" (Cam1 PORNIRE)"); break;
        case RECORD_CAM1_OFF: Serial.print(" (Cam1 OPRIRE)");  break;
        case RECORD_CAM2_ON:  Serial.print(" (Cam2 PORNIRE)"); break;
        case RECORD_CAM2_OFF: Serial.print(" (Cam2 OPRIRE)");  break;
      }
      Serial.print(": ");
      if (sta == 0x01)       Serial.println("ANTRENAT ✓");
      else if (sta == 0x00)  Serial.println("NEANTRENAT -> train " + String(r));
      else                   Serial.printf("STA=0x%02X\n", sta);
      i += 2;
    }
  } else {
    Serial.println("[INFO] Raspuns neasteptat - incearca din nou.");
  }
}

// ============================================================
//  VERIFICA RECUNOSCATORUL ACTIV
// ============================================================
void verificaRecunoscator() {
  Serial.println("\n[VCHECK] Recunoscator activ...");
  while (Serial2.available()) Serial2.read();

  vrSend(0x01, nullptr, 0);
  int ret = vrCitesteFrame(VR_TIMEOUT_RAPID);
  if (ret < 0) { Serial.println("[EROARE] Timeout!"); return; }
  afiseazaFrame("[RX]", ret);

  if (vrBuf[2] == 0x01) {
    uint8_t rvn = vrBuf[3];
    Serial.printf("Recorduri valide in recunoscator: %d\n", rvn);
    for (int i = 0; i < 7; i++) {
      uint8_t rec = vrBuf[4 + i];
      if (rec != 0xFF) {
        Serial.printf("  Slot %d: Record %d", i, rec);
        switch (rec) {
          case RECORD_CAM1_ON:  Serial.print(" (Cam1 PORNIRE)"); break;
          case RECORD_CAM1_OFF: Serial.print(" (Cam1 OPRIRE)");  break;
          case RECORD_CAM2_ON:  Serial.print(" (Cam2 PORNIRE)"); break;
          case RECORD_CAM2_OFF: Serial.print(" (Cam2 OPRIRE)");  break;
        }
        Serial.println();
      }
    }
    if (rvn == 0) Serial.println("Recunoscatorul e GOL! Foloseste 'load'.");
  }
}

// ============================================================
//  TEST RECUNOASTERE - 20 secunde
// ============================================================
void testeazaRecunoastere() {
  Serial.println("\n================================================");
  Serial.println("  MOD TEST - Ascult 20 secunde...");
  Serial.println("  Vorbeste una din comenzile antrenate!");
  Serial.println("  Apasa Enter pentru a opri.");
  Serial.println("================================================\n");

  uint8_t recs[] = {0x00, 0x01, 0x02, 0x03};
  vrSend(0x30, recs, 4);
  delay(400);
  while (Serial2.available()) Serial2.read();

  unsigned long start = millis();
  int detectii = 0;
  unsigned long ultimPunct = 0;

  while (millis() - start < 20000) {
    if (Serial.available()) { Serial.read(); break; }

    int ret = vrCitesteFrame(500);
    if (ret > 0) {
      afiseazaFrame("\n[FRAME]", ret);

      // CMD=0x0D = voice recognized
      // Format: AA 07 0D 00 GRPM R RI SIGLEN 0A
      if (vrBuf[2] == 0x0D) {
        detectii++;
        // R e la pozitia 5 (0-indexed): AA LEN CMD 00 GRPM R
        uint8_t rec = (vrBufLen >= 7) ? vrBuf[5] : vrBuf[4];
        Serial.printf("[DETECTAT] Record %d", rec);
        switch (rec) {
          case RECORD_CAM1_ON:  Serial.print(" -> Camera 1 PORNIRE"); break;
          case RECORD_CAM1_OFF: Serial.print(" -> Camera 1 OPRIRE");  break;
          case RECORD_CAM2_ON:  Serial.print(" -> Camera 2 PORNIRE"); break;
          case RECORD_CAM2_OFF: Serial.print(" -> Camera 2 OPRIRE");  break;
          default:              Serial.printf(" -> Record %d", rec);  break;
        }
        Serial.println(" ✓");
      }
    }

    if (millis() - ultimPunct > 1000) { Serial.print("."); ultimPunct = millis(); }
  }

  Serial.printf("\n\n[TEST INCHEIAT] Detectate: %d comenzi.\n", detectii);
  if (detectii == 0) {
    Serial.println("Sfaturi:");
    Serial.println("  1. 'check'  -> verifica antrenarea");
    Serial.println("  2. 'vcheck' -> verifica recunoscatorul activ");
    Serial.println("  3. Vorbeste la 10-20cm de microfon, camera linistita");
  }
}

// ============================================================
//  STERGE RECORD
// ============================================================
void stergeRecord(int nr) {
  Serial.printf("\n[CLEAR] Sterg Record %d...", nr);
  while (Serial2.available()) Serial2.read();
  uint8_t rec = (uint8_t)nr;
  vrSend(0x10, &rec, 1);
  int ret = vrCitesteFrame(VR_TIMEOUT_RAPID);
  afiseazaFrame(" [RX]", ret > 0 ? ret : 0);
}

// ============================================================
//  ANTRENEAZA TOATE 4
// ============================================================
void antreneazaTot() {
  Serial.println("\n================================================");
  Serial.println("  ANTRENARE COMPLETA - 4 comenzi");
  Serial.println("  Comenzi recomandate (sub 1.5 secunde):");
  Serial.println("    Record 0: ex. 'lumina unu'");
  Serial.println("    Record 1: ex. 'stop unu'");
  Serial.println("    Record 2: ex. 'lumina doi'");
  Serial.println("    Record 3: ex. 'stop doi'");
  Serial.println("================================================");
  delay(2000);
  for (int i = 0; i < 4; i++) {
    antreneazaRecord(i);
    if (i < 3) { Serial.println("\nUrmatorul in 3 secunde..."); delay(3000); }
  }
  Serial.println("\n[GATA] Urmatorul pas: check -> load -> test");
}

// ============================================================
//  PROCESEAZA COMANDA
// ============================================================
void proceseazaComanda(String cmd) {
  cmd.trim();
  String c = cmd; c.toLowerCase();
  if (c == "help")               afiseazaMeniu();
  else if (c == "load")          incarcaRecorduri();
  else if (c == "test")          testeazaRecunoastere();
  else if (c == "check")         verificaRecorduri();
  else if (c == "vcheck")        verificaRecunoscator();
  else if (c == "train all")     antreneazaTot();
  else if (c.startsWith("train ")) {
    int nr = c.substring(6).toInt();
    if (nr >= 0 && nr <= 79) antreneazaRecord(nr);
    else Serial.println("[EROARE] Record 0-79");
  }
  else if (c.startsWith("clear ")) {
    int nr = c.substring(6).toInt();
    if (nr >= 0 && nr <= 79) stergeRecord(nr);
    else Serial.println("[EROARE] Record 0-79");
  }
  else { Serial.printf("[EROARE] Comanda: \"%s\" - scrie 'help'\n", cmd.c_str()); }
}

void afiseazaMeniu() {
  Serial.println("\n================================================");
  Serial.println("  COMENZI:");
  Serial.println("  train all   -> antreneaza toate 4 comenzile");
  Serial.println("  train 0-3   -> antreneaza un record specific");
  Serial.println("  load        -> incarca recordurile in recunoscator");
  Serial.println("  test        -> test recunoastere 20s");
  Serial.println("  check       -> verifica recordurile antrenate");
  Serial.println("  vcheck      -> verifica recunoscatorul activ");
  Serial.println("  clear N     -> sterge Record N");
  Serial.println("  help        -> acest meniu");
  Serial.println("================================================");
  Serial.println("START: 'train all' daca e prima oara");
}
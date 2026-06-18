import cv2
import numpy as np
import paho.mqtt.client as mqtt
import time
import urllib.request
import os
import sys

MQTT_BROKER  = "127.0.0.1"
MQTT_PORT    = 1883
MQTT_USER    = ""
MQTT_PASS    = ""
TOPIC_CAM1   = "home/cam1/cmd"
TOPIC_CAM2   = "home/cam2/cmd"

WEBCAM_INDEX = 0
CONFIDENCE   = 0.45
DEBOUNCE_SEC = 3.0

BASE_DIR      = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()
MODEL_DIR     = os.path.join(BASE_DIR, "models")
PROTOTXT_URL  = "https://raw.githubusercontent.com/chuanqi305/MobileNet-SSD/master/deploy.prototxt"
MODEL_URL     = "https://github.com/chuanqi305/MobileNet-SSD/raw/master/mobilenet_iter_73000.caffemodel"
PROTOTXT_PATH = os.path.join(MODEL_DIR, "MobileNetSSD_deploy.prototxt")
MODEL_PATH    = os.path.join(MODEL_DIR, "MobileNetSSD_deploy.caffemodel")

CLASSES = [
    "background", "aeroplane", "bicycle", "bird", "boat",
    "bottle", "bus", "car", "cat", "chair", "cow",
    "diningtable", "dog", "horse", "motorbike", "person",
    "pottedplant", "sheep", "sofa", "train", "tvmonitor"
]

ANIMAL_CLASSES = {"bird", "cat", "cow", "dog", "horse", "sheep"}
ANIMAL_INDEXES = {i for i, c in enumerate(CLASSES) if c in ANIMAL_CLASSES}
PERSON_INDEX   = CLASSES.index("person")   # Varianta 2: detectam si omul

mqtt_client      = None
animal_detectat  = False
animal_last_seen = 0.0
cameras_blocate  = False
last_mqtt_send   = 0.0

stats = {
    "fps": 0.0,
    "frame_count": 0,
    "detectii_total": 0,
    "mqtt_trimise": 0,
    "mqtt_conectat": False,
    "last_label": "",
    "last_confidence": 0.0,
    "rezolutie": "?",
    "persoana": False,
}

log_lines = []

def log(msg):
    ts = time.strftime("%H:%M:%S")
    linie = f"[{ts}] {msg}"
    print(linie)
    log_lines.append(linie)
    if len(log_lines) > 20:
        log_lines.pop(0)

def on_connect(client, userdata, flags, rc, properties=None):
    rc_val = rc if isinstance(rc, int) else rc.value
    if rc_val == 0:
        stats["mqtt_conectat"] = True
        log("[MQTT] Conectat la broker!")
    else:
        stats["mqtt_conectat"] = False
        log(f"[MQTT] Eroare conectare, cod: {rc_val}")

def on_disconnect(client, userdata, rc, properties=None):
    stats["mqtt_conectat"] = False
    log(f"[MQTT] Deconectat (cod {rc})")

def conecteaza_mqtt():
    global mqtt_client
    try:
        mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="AnimalDetector_PC")
    except AttributeError:
        mqtt_client = mqtt.Client(client_id="AnimalDetector_PC")
    mqtt_client.on_connect    = on_connect
    mqtt_client.on_disconnect = on_disconnect
    if MQTT_USER:
        mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        mqtt_client.loop_start()
        log(f"[MQTT] Conectare la {MQTT_BROKER}:{MQTT_PORT}...")
    except Exception as e:
        log(f"[MQTT] Nu se poate conecta: {e}")

def trimite_mqtt(blocat: bool):
    global cameras_blocate, last_mqtt_send
    val   = "1" if blocat else "0"
    stare = "BLOCAT" if blocat else "NORMAL"
    try:
        mqtt_client.publish(TOPIC_CAM1, val, qos=1)
        mqtt_client.publish(TOPIC_CAM2, val, qos=1)
        cameras_blocate = blocat
        last_mqtt_send  = time.time()
        stats["mqtt_trimise"] += 1
        log(f"[MQTT] cam1/cmd={val}, cam2/cmd={val}  ({stare})")
    except Exception as e:
        log(f"[MQTT] Eroare trimitere: {e}")

def descarca_modele():
    os.makedirs(MODEL_DIR, exist_ok=True)
    if not os.path.exists(PROTOTXT_PATH):
        log("[MODEL] Descarc prototxt...")
        try:
            urllib.request.urlretrieve(PROTOTXT_URL, PROTOTXT_PATH)
            log("[MODEL] prototxt OK")
        except Exception as e:
            log(f"[MODEL] EROARE prototxt: {e}")
            return False
    if not os.path.exists(MODEL_PATH):
        log("[MODEL] Descarc caffemodel (~23MB)...")
        try:
            def progress(count, block_size, total_size):
                pct = int(count * block_size * 100 / total_size)
                sys.stdout.write(f"\r  Progres: {pct}%   ")
                sys.stdout.flush()
            urllib.request.urlretrieve(MODEL_URL, MODEL_PATH, reporthook=progress)
            print()
            log("[MODEL] caffemodel OK")
        except Exception as e:
            log(f"[MODEL] EROARE caffemodel: {e}")
            return False
    return True

def deseneaza_overlay(frame, detectii, persoane=None):
    h, w = frame.shape[:2]
    acum = time.time()

    overlay = frame.copy()
    cv2.rectangle(overlay, (0, 0), (w, 90), (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.7, frame, 0.3, 0, frame)

    cv2.putText(frame, "DETECTOR ANIMALE",
                (10, 30), cv2.FONT_HERSHEY_DUPLEX, 0.9, (0, 220, 255), 2)

    mqtt_col = (0, 255, 0) if stats["mqtt_conectat"] else (0, 0, 255)
    mqtt_txt = "MQTT: CONECTAT" if stats["mqtt_conectat"] else "MQTT: DECONECTAT"
    cv2.putText(frame, mqtt_txt,       (10, 58),  cv2.FONT_HERSHEY_SIMPLEX, 0.52, mqtt_col, 1)
    cv2.putText(frame, f"FPS: {stats['fps']:.1f}", (230, 58), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (200,200,200), 1)
    cv2.putText(frame, f"Res: {stats['rezolutie']}", (340, 58), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (200,200,200), 1)
    cv2.putText(frame, f"Detectii: {stats['detectii_total']}", (530, 58), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (200,200,200), 1)
    cv2.putText(frame, f"MQTT trimise: {stats['mqtt_trimise']}", (700, 58), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (200,200,200), 1)

    stare_txt = "CAMERE: BLOCATE" if cameras_blocate else "CAMERE: NORMALE"
    stare_col = (0, 80, 255)  if cameras_blocate else (0, 255, 100)
    cv2.rectangle(frame, (w - 280, 5), (w - 5, 42), (30, 30, 30), -1)
    cv2.putText(frame, stare_txt, (w - 275, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.6, stare_col, 2)

    if detectii:
        if int(acum * 2) % 2 == 0:
            label_up = stats["last_label"].upper()
            txt = f"!!! {label_up} DETECTAT !!!"
            tw, _ = cv2.getTextSize(txt, cv2.FONT_HERSHEY_SIMPLEX, 0.85, 2)[0]
            cv2.rectangle(frame, (w//2 - tw//2 - 10, 95), (w//2 + tw//2 + 10, 138), (0, 50, 180), -1)
            cv2.putText(frame, txt, (w//2 - tw//2, 128),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.85, (0, 220, 255), 2)
    else:
        if animal_last_seen > 0:
            elapsed = acum - animal_last_seen
            if elapsed < DEBOUNCE_SEC * 3:
                cv2.putText(frame, f"Ultima detectie: {elapsed:.1f}s in urma",
                            (w//2 - 190, 120), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (150,150,150), 1)

    for (label, confidence, box) in detectii:
        x, y, x2, y2 = box
        cv2.rectangle(frame, (x, y), (x2, y2), (0, 165, 255), 3)
        label_txt = f"{label}: {confidence:.0%}"
        (tw, th), _ = cv2.getTextSize(label_txt, cv2.FONT_HERSHEY_SIMPLEX, 0.7, 2)
        cv2.rectangle(frame, (x, y - th - 10), (x + tw + 8, y), (0, 100, 200), -1)
        cv2.putText(frame, label_txt, (x + 4, y - 5),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
        lc = 20
        for px, py, dx, dy in [(x,y,1,0),(x,y,0,1),(x2,y,-1,0),(x2,y,0,1),
                                (x,y2,1,0),(x,y2,0,-1),(x2,y2,-1,0),(x2,y2,0,-1)]:
            cv2.line(frame, (px, py), (px + dx*lc, py + dy*lc), (0, 255, 255), 2)

    # Varianta 2: deseneaza si persoanele detectate (verde)
    if persoane:
        for (label, confidence, box) in persoane:
            x, y, x2, y2 = box
            cv2.rectangle(frame, (x, y), (x2, y2), (0, 220, 0), 3)
            label_txt = f"{label}: {confidence:.0%}"
            (tw, th), _ = cv2.getTextSize(label_txt, cv2.FONT_HERSHEY_SIMPLEX, 0.7, 2)
            cv2.rectangle(frame, (x, y - th - 10), (x + tw + 8, y), (0, 150, 0), -1)
            cv2.putText(frame, label_txt, (x + 4, y - 5),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)

    # Indicator de mod (de ce sunt sau nu blocate camerele)
    if stats.get("persoana"):
        mod_txt, mod_col = "MOD: NORMAL (om prezent)", (0, 220, 0)
    elif cameras_blocate:
        mod_txt, mod_col = "MOD: VEGHE (animal singur)", (0, 80, 255)
    else:
        mod_txt, mod_col = "MOD: NORMAL", (200, 200, 200)
    cv2.putText(frame, mod_txt, (10, 80), cv2.FONT_HERSHEY_SIMPLEX, 0.52, mod_col, 1)

    log_h = min(len(log_lines), 8) * 20 + 10
    log_y = h - log_h - 5
    overlay2 = frame.copy()
    cv2.rectangle(overlay2, (0, log_y - 5), (w, h), (15, 15, 15), -1)
    cv2.addWeighted(overlay2, 0.75, frame, 0.25, 0, frame)
    for i, linie in enumerate(log_lines[-8:]):
        col = (100,255,100) if "[MQTT]" in linie else \
              (0,200,255)   if "DETECTAT" in linie else \
              (50,50,255)   if "EROARE" in linie else (180,180,180)
        cv2.putText(frame, linie[-100:], (8, log_y + 15 + i * 20),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.42, col, 1)

    if animal_last_seen > 0 and not cameras_blocate:
        elapsed = acum - animal_last_seen
        if elapsed < DEBOUNCE_SEC:
            pct   = elapsed / DEBOUNCE_SEC
            bar_w = int(w * pct)
            cv2.rectangle(frame, (0, h - log_h - 12), (bar_w, h - log_h - 7), (0, 165, 255), -1)
            cv2.putText(frame, f"Debounce: {elapsed:.1f}/{DEBOUNCE_SEC:.0f}s",
                        (10, h - log_h - 14), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (200,150,50), 1)
    return frame

def main():
    global animal_detectat, animal_last_seen, cameras_blocate

    log("=" * 50)
    log(" DETECTOR ANIMALE cu MQTT")
    log("=" * 50)
    log(f"Broker: {MQTT_BROKER}:{MQTT_PORT}")
    log(f"Animale detectate: {', '.join(sorted(ANIMAL_CLASSES))}")
    log(f"Prag detectie: {CONFIDENCE:.0%}  |  Debounce: {DEBOUNCE_SEC}s")

    log("Verific modelele...")
    if not descarca_modele():
        log("EROARE: Nu s-au putut descarca modelele!")
        input("Enter pentru iesire...")
        return

    log("Incarc modelul MobileNet SSD...")
    try:
        net = cv2.dnn.readNetFromCaffe(PROTOTXT_PATH, MODEL_PATH)
        net.setPreferableBackend(cv2.dnn.DNN_BACKEND_DEFAULT)
        net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
        log("Model incarcat pe CPU")
    except Exception as e:
        log(f"EROARE model: {e}")
        input("Enter pentru iesire...")
        return

    conecteaza_mqtt()
    time.sleep(1)

    log(f"Deschid webcam (index {WEBCAM_INDEX})...")
    cap = cv2.VideoCapture(WEBCAM_INDEX)
    if not cap.isOpened():
        log(f"EROARE: Nu pot deschide webcam {WEBCAM_INDEX}!")
        input("Enter pentru iesire...")
        return

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
    real_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    real_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    stats["rezolutie"] = f"{real_w}x{real_h}"
    log(f"Webcam: {real_w}x{real_h}")
    log("Apasa Q=iesire  B=blocare manuala  N=normal  S=screenshot")
    log("-" * 50)

    fps_timer   = time.time()
    fps_counter = 0

    cv2.namedWindow("Detector Animale", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("Detector Animale", 1280, 720)

    while True:
        ret, frame = cap.read()
        if not ret:
            log("EROARE: Nu pot citi frame!")
            break

        stats["frame_count"] += 1
        fps_counter += 1
        acum = time.time()

        if acum - fps_timer >= 1.0:
            stats["fps"] = fps_counter / (acum - fps_timer)
            fps_counter  = 0
            fps_timer    = acum

        h, w = frame.shape[:2]
        blob = cv2.dnn.blobFromImage(
            cv2.resize(frame, (300, 300)), 0.007843, (300, 300), 127.5)
        net.setInput(blob)
        detectii_raw = net.forward()

        detectii_animale = []
        persoane = []
        for i in range(detectii_raw.shape[2]):
            confidence = float(detectii_raw[0, 0, i, 2])
            class_id   = int(detectii_raw[0, 0, i, 1])
            if confidence < CONFIDENCE:
                continue
            box = detectii_raw[0, 0, i, 3:7] * np.array([w, h, w, h])
            x, y, x2, y2 = box.astype(int)
            x  = max(0, x);  y  = max(0, y)
            x2 = min(w, x2); y2 = min(h, y2)
            if class_id == PERSON_INDEX:
                persoane.append(("Persoana", confidence, (x, y, x2, y2)))
            elif class_id in ANIMAL_INDEXES:
                label = CLASSES[class_id].capitalize()
                detectii_animale.append((label, confidence, (x, y, x2, y2)))
                stats["last_confidence"] = confidence
                stats["last_label"]      = label

        persoana_prezenta = len(persoane) > 0
        stats["persoana"] = persoana_prezenta

        # VARIANTA 2: trecem in veghe (20%) DOAR daca exista animal SI NU exista om in cadru.
        # Daca apare un om, sistemul revine la functionare normala (PIR + daylight harvesting).
        trebuie_blocat = bool(detectii_animale) and not persoana_prezenta

        if trebuie_blocat:
            if not animal_detectat:
                animal_detectat = True
                stats["detectii_total"] += 1
                log(f"{stats['last_label'].upper()} DETECTAT (fara om)! {stats['last_confidence']:.1%}")
            animal_last_seen = acum
            if not cameras_blocate:
                log("Trimit BLOCARE ambele camere (animal singur)...")
                trimite_mqtt(blocat=True)
        else:
            if animal_detectat:
                if persoana_prezenta:
                    # om langa animal -> deblocam imediat, e cineva acasa
                    animal_detectat = False
                    log("Om prezent langa animal -> functionare normala")
                    if cameras_blocate:
                        log("Trimit DEBLOCARE ambele camere...")
                        trimite_mqtt(blocat=False)
                elif acum - animal_last_seen >= DEBOUNCE_SEC:
                    animal_detectat = False
                    log(f"Animal disparut (debounce {DEBOUNCE_SEC}s)")
                    if cameras_blocate:
                        log("Trimit DEBLOCARE ambele camere...")
                        trimite_mqtt(blocat=False)

        frame = deseneaza_overlay(frame, detectii_animale, persoane)
        cv2.imshow("Detector Animale", frame)

        key = cv2.waitKey(1) & 0xFF
        if key in (ord('q'), 27):
            log("Iesire.")
            break
        elif key == ord('b'):
            log("Blocare manuala (B)")
            trimite_mqtt(blocat=True)
        elif key == ord('n'):
            log("Deblocare manuala (N)")
            trimite_mqtt(blocat=False)
        elif key == ord('s'):
            fname = f"screenshot_{int(acum)}.jpg"
            cv2.imwrite(fname, frame)
            log(f"Screenshot: {fname}")

    log("Inchid...")
    if cameras_blocate:
        trimite_mqtt(blocat=False)
        time.sleep(0.5)
    cap.release()
    cv2.destroyAllWindows()
    mqtt_client.loop_stop()
    mqtt_client.disconnect()
    log("Inchis.")

if __name__ == "__main__":
    main()
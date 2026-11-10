import time
import re
import cv2
import cv2.aruco as aruco
import math
import mediapipe as mp
import speech_recognition as sr
import sounddevice as sd
import serial

# ==================== ГЛОБАЛЬНЫЕ НАСТРОЙКИ РОБОТА ====================
WAKE_WORD = "робот"
SAMPLE_RATE = 16000
DURATION = 5  # Длина аудиозаписи (секунд)

# Реальный физический размер стороны напечатанного рисунка-маркера на руке (в метрах)
# Обязательно измерьте линейкой ваш распечатанный маркер и укажите точно
MARKER_SIZE = 0.08  

# Порт подключения к Arduino (в Linux на Orange Pi обычно /dev/ttyACM0)
PORT_NAME = '/dev/ttyACM0' 
# =====================================================================

# Инициализация ИИ-детектора рук MediaPipe
mp_hands = mp.solutions.hands
hands = mp_hands.Hands(max_num_hands=1, min_detection_confidence=0.7)

# Настройка словаря Aruco-маркеров (используем стандартный 4x4)
aruco_dict = aruco.getPredefinedDictionary(aruco.DICT_4X4_50)
aruco_params = aruco.DetectorParameters()
detector = aruco.ArucoDetector(aruco_dict, aruco_params)

# Параметры камеры (калибровка по умолчанию для стандартных USB-камер)
cam_matrix = cv2.Mat(3, 3, cv2.CV_64F, [[650.0, 0.0, 320.0], [0.0, 650.0, 240.0], [0.0, 0.0, 1.0]])
dist_coeffs = cv2.Mat(1, 5, cv2.CV_64F, [0.0, 0.0, 0.0, 0.0, 0.0])

try:
    print(f"[ЯДРО] Подключение к аппаратной платформе на {PORT_NAME}...")
    arduino = serial.Serial(port=PORT_NAME, baudrate=115200, timeout=1)
    time.sleep(2)
    print("[ЯДРО] Связь с Arduino успешно установлена!\n")
except Exception as e:
    print(f"[ЯДРО] Ошибка подключения по USB: {e}. Режим симуляции.")
    arduino = None

def start_visual_navigation_and_delivery(serial_connection, cell):
    """Функция активирует камеру, находит рисунок-маркер на руке, строит 
    вектор движения, доводит робота до человека и подает предмет по ладони."""
    print("[КАМЕРА] Включение видеосистемы навигации...")
    cap = cv2.VideoCapture(0) # Основная USB-камера робота
    
    cam_width = 640
    cap.set(3, cam_width)
    frame_center_x = cam_width // 2

    # Шаг 1: Извлечение из ячейки (шлем команду в Arduino)
    if serial_connection and serial_connection.is_open:
        serial_connection.write(f"CELL:{cell}\n".encode('utf-8'))
    print(f"[НАВИГАЦИЯ] Жду извлечения предмета из ячейки {cell}...")
    time.sleep(2.0) 

    print("[НАВИГАЦИЯ] Начинаю поиск рисунка-маркера на руке человека...")
    
    navigating = True
    while navigating:
        ret, frame = cap.read()
        if not ret:
            continue

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        corners, ids, rejected = detector.detectMarkers(gray)

        # УЛЬТРАЗВУКОВОЙ ТОРМОЗ (Опрос Arduino, не уперся ли бампер)
        if serial_connection and serial_connection.in_waiting > 0:
            response = serial_connection.readline().decode('utf-8').strip()
            if "BUMPER_STOP" in response:
                print("[НАВИГАЦИЯ] Бампер обнаружил человека в упор. Остановка шасси.")
                if serial_connection and serial_connection.is_open:
                    serial_connection.write(b"DRIVE:0:0\n") # Стоп моторы
                navigating = False
                break

        if ids is not None:
            # Вычисляем точное пространственное положение рисунка-маркера (3D-вектор)
            rvecs, tvecs, _ = aruco.estimatePoseSingleMarkers(corners, MARKER_SIZE, cam_matrix, dist_coeffs)
            
            # tvecs содержит координаты [X, Y, Z] относительно центра объектива в метрах
            x_m = tvecs[0][0][0] # Смещение влево/вправо
            z_m = tvecs[0][0][2] # Расстояние до маркера прямо по курсу
            
            # Вычисляем угол вектора на человека в радианах
            angle_rad = math.atan2(x_m, z_m)
            
            print(f"[ИИ ВЕКТОР] Дистанция: {z_m:.2f}м, Угол отклонения: {math.degrees(angle_rad):.1f} градусов")

            # Расчет скоростей движения
            max_speed = 120
            min_speed = 40
            
            # Плавное замедление при приближении ближе 1.5 метров
            if z_m < 1.5:
                move_speed = min_speed + (max_speed - min_speed) * (z_m / 1.5)
                move_speed = max(min_speed, min(max_speed, move_speed))
            else:
                move_speed = max_speed

            # Разложение вектора скорости на компоненты колес Илона
            forward = int(move_speed * math.cos(angle_rad))
            strafe = int(move_speed * math.sin(angle_rad))

            # Отправляем векторные скорости прямо в моторы Arduino
            if serial_connection and serial_connection.is_open:
                motor_cmd = f"DRIVE:{forward}:{strafe}\n"
                serial_connection.write(motor_cmd.encode('utf-8'))
        else:
            # Если рисунок потерялся из виду — плавно тормозим
            if serial_connection and serial_connection.is_open:
                serial_connection.write(b"DRIVE:0:0\n")

        time.sleep(0.05) # Обновление вектора 20 раз в секунду

    # Шаг 3: Робот у ног человека. Переключаемся на ИИ-трекинг ладони через MediaPipe
    print("[КАМЕРА] Переключение в режим ювелирной подачи по ладони...")
    tracking_active = True
    while tracking_active:
        ret, frame = cap.read()
        if not ret:
            continue

        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        results = hands.process(rgb_frame)

        if results.multi_hand_landmarks:
            for hand_landmarks in results.multi_hand_landmarks:
                palm_x = int(hand_landmarks.landmark[9].x * cam_width)
                error_x = palm_x - frame_center_x
                
                if abs(error_x) > 20: 
                    if serial_connection and serial_connection.is_open:
                        if error_x > 0: serial_connection.write(b"ARM:RIGHT\n")
                        else: serial_connection.write(b"ARM:LEFT\n")
                else:
                    print("[КАМЕРА] Ладонь отцентрирована. Подаю предмет.")
                    if serial_connection and serial_connection.is_open:
                        serial_connection.write(b"ARM:EXTEND\n")
                    tracking_active = False  
                    break
        time.sleep(0.03)
        
    cap.release()

# Инициализация распознавания речи
recognizer = sr.Recognizer()
print(f"[РЕЧЬ] Система запущена. Жду фразу со словом '{WAKE_WORD}'...")

try:
    while True:
        audio_data = sd.rec(int(DURATION * SAMPLE_RATE), samplerate=SAMPLE_RATE, channels=1, dtype='int16')
        sd.wait()
        
        audio_bytes = audio_data.tobytes()
        audio_source = sr.AudioData(audio_bytes, SAMPLE_RATE, 2)
        
        try:
            text = recognizer.recognize_google(audio_source, language="ru-RU").lower().strip()
            if not text: continue
            print(f"[Слышу]: {text}")
            
            if WAKE_WORD in text:
                numbers = re.findall(r'\d+', text)
                if numbers:
                    cell_number = int(numbers[0])
                    if 1 <= cell_number <= 16:
                        # Запуск единого визуального цикла
                        start_visual_navigation_and_delivery(arduino, cell_number)
                        
                        # Ждем маркер финиша от Arduino
                        while True:
                            if arduino and arduino.in_waiting > 0:
                                res = arduino.readline().decode('utf-8').strip()
                                if "DONE" in res:
                                    print("[ИИ] Выдача успешно завершена. Робот готов к новым командам.\n")
                                    break
                            time.sleep(0.1)
        except sr.UnknownValueError: pass
        except sr.RequestError as e: time.sleep(2)
        time.sleep(0.1)
except KeyboardInterrupt:
    if arduino and arduino.is_open: arduino.close()

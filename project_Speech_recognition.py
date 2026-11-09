import time
import re
import cv2
import mediapipe as mp
import speech_recognition as sr
import sounddevice as sd
import serial

# ==================== ГЛОБАЛЬНЫЕ НАСТРОЙКИ РОБОТА ====================
WAKE_WORD = "робот"
SAMPLE_RATE = 16000
DURATION = 5  # Длина аудиозаписи (секунд)

# Порт подключения к Arduino (в Linux на Orange Pi обычно /dev/ttyACM0 или /dev/ttyUSB0)
PORT_NAME = '/dev/ttyACM0' 
# =====================================================================

# Инициализация ИИ-детектора рук MediaPipe
mp_hands = mp.solutions.hands
hands = mp_hands.Hands(max_num_hands=1, min_detection_confidence=0.7)

# Инициализация связи с контроллером движений по USB
try:
    print(f"[ЯДРО] Подключение к аппаратной платформе на {PORT_NAME}...")
    arduino = serial.Serial(port=PORT_NAME, baudrate=115200, timeout=1)
    time.sleep(2)  # Пауза на перезагрузку Arduino
    print("[ЯДРО] Связь с Arduino успешно установлена!\n")
except Exception as e:
    print(f"[ЯДРО] ❌ Ошибка подключения по USB: {e}")
    print("[ЯДРО] ВНИМАНИЕ: Скрипт запущен в режиме СИМУЛЯЦИИ (без отправки команд)!")
    arduino = None

def start_camera_hand_tracking(serial_connection):
    """Функция активируется, когда робот приехал к ногам человека. 
    Включает зрение камеры и выравнивает манипулятор по ладони."""
    print("[КАМЕРА] Активация видеопотока с манипулятора...")
    cap = cv2.VideoCapture(0)  # 0 - ID USB-камеры на конце роборуки
    
    cam_width = 640
    cap.set(3, cam_width)
    frame_center_x = cam_width // 2  # Центральный пиксель кадра (320)

    print("[КАМЕРА] ИИ ищет ладонь человека для точного сброса...")
    
    tracking_active = True
    while tracking_active:
        ret, frame = cap.read()
        if not ret:
            continue

        # Переводим кадр в RGB формат для нейросети MediaPipe
        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        results = hands.process(rgb_frame)

        if results.multi_hand_landmarks:
            for hand_landmarks in results.multi_hand_landmarks:
                # Извлекаем координаты точки №9 (анатомический центр ладони)
                palm_mcp = hand_landmarks.landmark[9]
                
                # Переводим нормализованную ИИ-координату (0.0 - 1.0) в пиксели кадра
                palm_x = int(palm_mcp.x * cam_width)
                
                # Вычисляем ошибку смещения ладони от центра объектива
                error_x = palm_x - frame_center_x
                
                # Если рука человека не по центру, шлем команды микро-корректировки в Arduino
                if abs(error_x) > 20: 
                    if serial_connection and serial_connection.is_open:
                        if error_x > 0:
                            serial_connection.write(b"ARM:RIGHT\n")
                        else:
                            serial_connection.write(b"ARM:LEFT\n")
                else:
                    # Ладонь человека отцентрирована! Вытягиваем манипулятор вперед
                    print("[КАМЕРА] 🔥 Цель зафиксирована по центру! Выдвигаю руку.")
                    if serial_connection and serial_connection.is_open:
                        serial_connection.write(b"ARM:EXTEND\n")
                    tracking_active = False  
                    break
                    
        time.sleep(0.03)  # Частота обработки ~30 кадров/сек
        
    cap.release()

# Инициализация распознавателя речи
recognizer = sr.Recognizer()
print("[РЕЧЬ] Мозг робота инициализирован. Ожидание аудиосигнала.")
print(f"[РЕЧЬ] Четко произнесите фразу, например: '{WAKE_WORD} 5' или '{WAKE_WORD} вези ячейку 12'...")

try:
    while True:
        # Циклическая запись коротких аудио-отрезков
        audio_data = sd.rec(int(DURATION * SAMPLE_RATE), samplerate=SAMPLE_RATE, channels=1, dtype='int16')
        sd.wait()
        
        audio_bytes = audio_data.tobytes()
        audio_source = sr.AudioData(audio_bytes, SAMPLE_RATE, 2)
        
        try:
            # Распознавание голоса через облачный API Google
            text = recognizer.recognize_google(audio_source, language="ru-RU").lower().strip()
            if not text:
                continue
                
            print(f"[Слышу]: {text}")
            
            if WAKE_WORD in text:
                print(f"[ТРИГГЕР]: Обнаружено кодовое имя '{WAKE_WORD}'!")
                
                # Поиск чисел в распознанном предложении
                numbers = re.findall(r'\d+', text)
                
                if numbers:
                    cell_number = int(numbers[0])
                    
                    # Валидация ячеек (1 - 16)
                    if 1 <= cell_number <= 16:
                        print(f"[ИИ] Команда одобрена. Целевой объект: Ячейка №{cell_number}.")
                        
                        if arduino and arduino.is_open:
                            # Передаем стартовую команду в Arduino
                            command = f"CELL:{cell_number}\n"
                            arduino.write(command.encode('utf-8'))
                            print(f"[USB -> Arduino]: Передана инструкция: {command.strip()}")
                            
                            # Переходим в режим ожидания сигналов от аппаратного шасси
                            print("[ИИ] Робот уехал на склад. Ожидание завершения фазы навигации...")
                            while True:
                                if arduino.in_waiting > 0:
                                    response = arduino.readline().decode('utf-8').strip()
                                    
                                    # Робот доехал по UWB к ногам человека
                                    if "ARRIVED" in response:
                                        print("[ИИ] Робот запарковался у ног человека. Запуск ИИ-зрения камеры...")
                                        start_camera_hand_tracking(arduino)
                                    
                                    # Весь цикл завершен, человек забрал деталь
                                    if "DONE" in response:
                                        print("[ИИ] ✅ Операция выдачи успешно завершена. Робот возвращается в режим ожидания.")
                                        break
                                time.sleep(0.1)
                        else:
                            print(f"[СИМУЛЯЦИЯ] Робот поехал бы извлекать ячейку {cell_number}...")
                    else:
                        print(f"[ОШИБКА] Номер ячейки {cell_number} вне диапазона склада! Допустимо только 1-16.")
                else:
                    print("[ПРЕДУПРЕЖДЕНИЕ] Имя робота названо, но номер ячейки не обнаружен.")
                    
        except sr.UnknownValueError:
            pass  # Игнорируем фоновые шумы и тишину в комнате
        except sr.RequestError as e:
            print(f"[СЕТЬ] Ошибка связи с сервером. Проверьте интернет/Wi-Fi: {e}")
            time.sleep(2)
            
        time.sleep(0.1)

except KeyboardInterrupt:
    print("\n[ВЫХОД] Программа ядра остановлена пользователем.")
    if arduino and arduino.is_open:
        arduino.close()

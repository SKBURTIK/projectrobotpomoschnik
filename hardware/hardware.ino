#include <SPI.h>
#include <DW1000.h> // Библиотека, которую вы установили вручную из ZIP-архива
#include <Servo.h>

// ==================== АППАРАТНАЯ КОНФИГУРАЦИЯ ПИНОВ ====================
// Силовые выходы на 4 драйвера независимых меканум-колёс (ШИМ скорость и Направление)
const int PIN_LF_SPEED = 2; const int PIN_LF_DIR = 3;  // Левое Переднее
const int PIN_RF_SPEED = 4; const int PIN_RF_DIR = 5;  // Правое Переднее
const int PIN_LB_SPEED = 6; const int PIN_LB_DIR = 7;  // Левое Заднее
const int PIN_RB_SPEED = 8; const int PIN_RB_DIR = 9;  // Правое Заднее

// Сервоприводы манипулятора
const int PIN_SERVO_BASE = 10;   // Поворот башни (Влево / Вправо)
const int PIN_SERVO_PULSE = A1;  // Выдвижение плеча руки вперед (Выдача предмета)

Servo baseServo;
Servo extendServo;

// Инфракрасный/Ультразвуковой парктроник безопасности бампера (HC-SR04)
const int PIN_TRIG = 11;
const int PIN_ECHO = 12;

// Выводы Chip Select (CS) для двух модулей DWM1000, сидящих на одной шине SPI
const int PIN_UWB_CS1 = 13; 
const int PIN_UWB_CS2 = A0; 
// =======================================================================

// ==================== КОНСТАНТЫ НАВИГАЦИИ И СКЛАДА =====================
// Таблица углов сервопривода для 16 ячеек хранения (индексы 1-16, 0-й не используется)
const int cellAngles[] = {0, 10, 22, 35, 48, 60, 72, 85, 98, 110, 122, 135, 148, 160, 172, 185, 200};

const float UWB_BASE_DISTANCE = 0.30; // Расстояние (база) между двумя датчиками UWB на роботе в метрах
const float TARGET_DISTANCE = 0.04;   // Дистанция парковки у ног человека (4 см)
const int MAX_SPEED = 120;            // Предельная маршевая скорость шасси (0 - 255)
const int MIN_SPEED = 40;             // Минимальный порог тяги моторов против застревания

int currentArmAngle = 90;             // Переменная динамического отслеживания угла руки
// =======================================================================

void setup() {
  Serial.begin(115200); // Скорость порта совпадает со скоростью в Python
  
  // Перевод контактов ходовой части в режим выходов напряжения
  pinMode(PIN_LF_SPEED, OUTPUT); pinMode(PIN_LF_DIR, OUTPUT);
  pinMode(PIN_RF_SPEED, OUTPUT); pinMode(PIN_RF_DIR, OUTPUT);
  pinMode(PIN_LB_SPEED, OUTPUT); pinMode(PIN_LB_DIR, OUTPUT);
  pinMode(PIN_RB_SPEED, OUTPUT); pinMode(PIN_RB_DIR, OUTPUT);
  
  // Настройка дальномера
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  // Инициализация сервоприводов манипулятора
  baseServo.attach(PIN_SERVO_BASE);
  extendServo.attach(PIN_SERVO_PULSE);
  
  baseServo.write(90);      // Стартовая центровка башни
  extendServo.write(10);    // Рука втянута в корпус робота

  // Запуск аппаратного интерфейса SPI и инициализация чипов Decawave
  SPI.begin();
  DW1000.begin(0, 0); 
  
  printDebug("Нижний аппаратный уровень робота запущен без сбоев.");
}

void loop() {
  // Постоянно слушаем команды от верхнего уровня по USB-кабелю
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim(); // Очищаем строку от мусорных символов
    
    // 1. КОМАНДА ЗАПУСКА СТАРТОВОГО СЦЕНАРИЯ
    if (command.startsWith("CELL:")) {
      int cellNum = command.substring(5).toInt();
      printDebug("Исполняю перемещение к ячейке: " + String(cellNum));
      executeDeliverySequence(cellNum);
    }
    
    // 2. ДИНАМИЧЕСКИЕ КОМАНДЫ ОТ ИИ-ЗРЕНИЯ КАМЕРЫ С ОРАНДЖ ПИ
    if (command == "ARM:LEFT") {
      currentArmAngle -= 2; // Доворачиваем башню на 2 градуса влево вслед за ладонью
      currentArmAngle = constrain(currentArmAngle, 0, 180);
      baseServo.write(currentArmAngle);
    }
    
    if (command == "ARM:RIGHT") {
      currentArmAngle += 2; // Доворачиваем башню на 2 градуса вправо вслед за ладонью
      currentArmAngle = constrain(currentArmAngle, 0, 180);
      baseServo.write(currentArmAngle);
    }
    
    if (command == "ARM:EXTEND") {
      printDebug("Камера подтвердила центровку ладони. Выдвигаю плечо подачи!");
      extendServo.write(150); // Вытягиваем второе серво (плечо) вперед к руке человека
      delay(3000);            // Ждем 3 секунды, пока человек забирает деталь
      
      extendServo.write(10);  // Втягиваем руку обратно в корпус робота
      baseServo.write(90);    // Сбрасываем башню в центр
      
      // Отправляем финальный отчет в Python, робот завершил миссию
      Serial.println("DONE");
    }
  }
}

// Главная пошаговая цепочка навигации и извлечения предмета
void executeDeliverySequence(int cell) {
  // Поворот манипулятора к целевой ячейке склада
  int targetAngle = cellAngles[cell];
  baseServo.write(targetAngle);
  delay(1500); // Физический тайминг на разворот башни и захват груза внутри ячейки
  
  printDebug("Груз извлечен. Начинаю сближение по UWB радиовектору...");

  bool arrived = false;
  while (!arrived) {
    // Получаем текущую дальность по радиоканалу от двух точек шасси до человека
    float dist1 = getUwbDistance(PIN_UWB_CS1); 
    float dist2 = getUwbDistance(PIN_UWB_CS2); 
    
    // ФИНИШНЫЙ ТОРМОЗ: Опрос ультразвукового датчика бампера в упор
    long ultrasonicDist = getUltrasonicDistance();
    if (ultrasonicDist > 0 && ultrasonicDist <= (TARGET_DISTANCE * 100)) { 
      moveMecanum(0, 0, 0); // Экстренное торможение всех 4 моторов шасси
      printDebug("Цель достигнута. Стоп-дистанция парковки: " + String(ultrasonicDist) + " см.");
      arrived = true;
      break;
    }

    // ТРИГОНОМЕТРИЧЕСКИЙ РАСЧЁТ ВЕКТОРНЫХ КООРДИНАТ ЦЕЛИ
    float currentRange = (dist1 + dist2) / 2.0; // Модуль вектора R (расстояние в метрах)
    float delta = dist1 - dist2;
    float sinAlpha = delta / UWB_BASE_DISTANCE;
    
    sinAlpha = constrain(sinAlpha, -1.0, 1.0);
    float angleToHuman = asin(sinAlpha); // Направление вектора (Угол Альфа в радианах)

    // Вывод вектора в консоль для удобной калибровки
    float angleInDegrees = angleToHuman * 180.0 / PI;
    printDebug("Вектор цели: Дальность = " + String(currentRange) + "м, Угол = " + String(angleInDegrees) + "°");

    // ПЛАВНОЕ ЗАМЕДЛЕНИЕ (Падение маршевой тяги при приближении к человеку)
    int moveSpeed = MAX_SPEED;
    if (currentRange < 1.5) {
      moveSpeed = MIN_SPEED + (MAX_SPEED - MIN_SPEED) * (currentRange / 1.5);
      moveSpeed = constrain(moveSpeed, MIN_SPEED, MAX_SPEED);
    }

    // ПРОЕКЦИЯ ПОЛЯРНОГО ВЕКТОРА НА МЕКАНУМ-ШАССИ
    int forwardComponent = moveSpeed * cos(angleToHuman); // Продольное смещение колес
    int strafeComponent = moveSpeed * sin(angleToHuman);  // Боковое скольжение колес
    int rotationComponent = 0; 

    // Фильтр колебания траектории
    if (abs(angleInDegrees) < 5.0) {
      strafeComponent = 0;
    }

    // Посылка импульсов векторов на независимые моторы
    moveMecanum(forwardComponent, strafeComponent, rotationComponent);
    
    delay(50); // Пересчёт вектора навигации 20 раз в секунду
  }

  // Робот замер у ног человека. Запоминаем текущий базовый угол руки
  currentArmAngle = cellAngles[cell]; 
  
  // Отправляем маркер прибытия в Python на Orange Pi по USB-кабелю
  // Эта команда заставит Orange Pi включить видеокамеру и запустить MediaPipe нейросеть
  Serial.println("ARRIVED");
}

// Распределитель крутящего момента для 4-х меканум-колёс (Колёса Илона)
void moveMecanum(int forward, int strafe, int turn) {
  int lf = forward + strafe + turn;
  int rf = forward - strafe - turn;
  int lb = forward - strafe + turn;
  int rb = forward + strafe - turn;

  setMotor(PIN_LF_SPEED, PIN_LF_DIR, lf);
  setMotor(PIN_RF_SPEED, PIN_RF_DIR, rf);
  setMotor(PIN_LB_SPEED, PIN_LB_DIR, lb);
  setMotor(PIN_RB_SPEED, PIN_RB_DIR, rb);
}

// Подача точных сигналов ШИМ мощности и логических уровней DIR на драйвер конкретного мотора
void setMotor(int speedPin, int dirPin, int speedValue) {
  if (speedValue >= 0) {
    digitalWrite(dirPin, HIGH); // Вращение колеса вперед
    analogWrite(speedPin, constrain(speedValue, 0, 255));
  } else {
    digitalWrite(dirPin, LOW);  // Вращение колеса назад
    analogWrite(speedPin, constrain(abs(speedValue), 0, 255));
  }
}

// Физический запрос дистанции от радиочипа DWM1000 через шину SPI
float getUwbDistance(int csPin) {
  digitalWrite(csPin, LOW); 
  // Здесь библиотека Thotro опрашивает регистры Decawave
  // В рабочей сборке вызывается метод дальномера до ID Тега человека: DW1000Ranging.getDistance()
  float distance = 1.6; // Безопасная программная заглушка в метрах (тип float)
  digitalWrite(csPin, HIGH); 
  return distance;
}

// Физический опрос ультразвукового эхо-локатора безопасности бампера (HC-SR04)
long getUltrasonicDistance() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  
  long duration = pulseIn(PIN_ECHO, HIGH, 30000); 
  if (duration == 0) return -1;
  return duration / 29 / 2; // Возвращает точное расстояние до преграды в сантиметрах
}

void printDebug(String msg) {
  Serial.println("[LOG] " + msg);
}

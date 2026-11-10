#include <Servo.h>

// ==================== АППАРАТНАЯ КОНФИГУРАЦИЯ ПИНОВ ====================
// Контакты управления 4-мя меканум-колёсами (ШИМ скорость и направление)
const int PIN_LF_SPEED = 2; const int PIN_LF_DIR = 3;  // Левое Переднее
const int PIN_RF_SPEED = 4; const int PIN_RF_DIR = 5;  // Правое Переднее
const int PIN_LB_SPEED = 6; const int PIN_LB_DIR = 7;  // Левое Заднее
const int PIN_RB_SPEED = 8; const int PIN_RB_DIR = 9;  // Правое Заднее

// Сервоприводы манипулятора
const int PIN_SERVO_BASE = 10;   // Поворот башни (Влево / Вправо)
const int PIN_SERVO_PULSE = A1;  // Выдвижение плеча руки вперед (Выдача предмета)

Servo baseServo;
Servo extendServo;

// Датчик парковки бампера безопасности "в упор" (HC-SR04)
const int PIN_TRIG = 11;
const int PIN_ECHO = 12;
// =======================================================================

const int cellAngles[] = {0, 10, 22, 35, 48, 60, 72, 85, 98, 110, 122, 135, 148, 160, 172, 185, 200};
int currentArmAngle = 90;

void setup() {
  Serial.begin(115200); // Открываем скоростной шнур связи с Orange Pi
  
  pinMode(PIN_LF_SPEED, OUTPUT); pinMode(PIN_LF_DIR, OUTPUT);
  pinMode(PIN_RF_SPEED, OUTPUT); pinMode(PIN_RF_DIR, OUTPUT);
  pinMode(PIN_LB_SPEED, OUTPUT); pinMode(PIN_LB_DIR, OUTPUT);
  pinMode(PIN_RB_SPEED, OUTPUT); pinMode(PIN_RB_DIR, OUTPUT);
  
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  baseServo.attach(PIN_SERVO_BASE);
  extendServo.attach(PIN_SERVO_PULSE);
  
  baseServo.write(90);      
  extendServo.write(10);    
}

void loop() {
  // НЕПРЕРЫВНАЯ ПОДСТРАХОВКА БАМПЕРА: парктроник опрашивается постоянно
  long ultrasonicDist = getUltrasonicDistance();
  if (ultrasonicDist > 0 && ultrasonicDist <= 4) { // 4 см до ноги
    // Если робот еще ехал, шлем аварийный стоп-сигнал в Orange Pi
    Serial.println("BUMPER_STOP");
  }

  // Прием исполнительных команд вектора скоростей от Orange Pi по USB
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim(); // Очищаем строку от мусорных символов
    
    // КОМАНДА 1: ИЗВЛЕЧЬ ИЗ ЯЧЕЙКИ
    if (command.startsWith("CELL:")) {
      int cellNum = command.substring(5).toInt();
      currentArmAngle = cellAngles[cellNum];
      baseServo.write(currentArmAngle); // Поворот руки к складу
    }
    
    // КОМАНДА 2: ДВИЖЕНИЕ ШАССИ (Принимает готовые разложенные скорости)
    if (command.startsWith("DRIVE:")) {
      // Парсим строку формата "DRIVE:forward:strafe"
      int firstColon = command.indexOf(':');
      int secondColon = command.indexOf(':', firstColon + 1);
      
      int forward = command.substring(firstColon + 1, secondColon).toInt();
      int strafe = command.substring(secondColon + 1).toInt();
      
      // Подаем мощности на колеса Илона
      moveMecanum(forward, strafe, 0);
    }
    
    // КОМАНДЫ 3: КОРРЕКТИРОВКА ОТ ИИ-ЗРЕНИЯ КАМЕРЫ
    if (command == "ARM:LEFT") {
      currentArmAngle -= 2;
      currentArmAngle = constrain(currentArmAngle, 0, 180);
      baseServo.write(currentArmAngle);
    }
    
    if (command == "ARM:RIGHT") {
      currentArmAngle += 2;
      currentArmAngle = constrain(currentArmAngle, 0, 180);
      baseServo.write(currentArmAngle);
    }
    
    if (command == "ARM:EXTEND") {
      moveMecanum(0, 0, 0); // На всякий случай полностью блокируем шасси
      extendServo.write(150); // Протягиваем руку вперед к человеку
      delay(3000);            // Время на забор детали
      
      extendServo.write(10);  // Втягиваем руку обратно
      baseServo.write(90);    // Сброс в центр
      
      Serial.println("DONE"); // Отчет в Python — робот свободен
    }
  }
}

// Распределитель крутящего момента для колес Илона (Mecanum)
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

void setMotor(int speedPin, int dirPin, int speedValue) {
  if (speedValue >= 0) {
    digitalWrite(dirPin, HIGH);
    analogWrite(speedPin, constrain(speedValue, 0, 255));
  } else {
    digitalWrite(dirPin, LOW);
    analogWrite(speedPin, constrain(abs(speedValue), 0, 255));
  }
}

long getUltrasonicDistance() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  
  long duration = pulseIn(PIN_ECHO, HIGH, 20000); 
  if (duration == 0) return -1;
  return duration / 29 / 2;
}

# Semáforo Inteligente

Projeto da disciplina **Sistemas Embarcados** (Automação e Conectividade), IFPE Campus Garanhuns.
Equipe: Natália Santos, Jamylle Geiza e Beatriz Moura.

## Descrição

Central de monitoramento e controle remoto de semáforos com tempos adaptativos. Uma plataforma
web (React + Node.js + MongoDB) cadastra os semáforos e publica a configuração de cada um via
**MQTT**. O firmware do **ESP32** (pasta `firmware-esp32`) recebe essa configuração, lê o sensor
de presença **HC-SR04** e aciona os LEDs do semáforo.

## Como o firmware funciona

1. Ao ligar, o ESP32 conecta ao Wi-Fi e ao broker MQTT (`broker.hivemq.com`) e assina o tópico `Ifpe/Semaforo/Semaforo1`.
2. A plataforma publica um JSON com os tempos de carro e pedestre (padrão e horário de pico), a janela de horário de pico e o status operacional. O ESP32 atualiza seus tempos ao receber a mensagem.
3. Os LEDs alternam verde → amarelo → vermelho usando `millis()` (sem `delay()`), para o semáforo não travar.
4. A cada 200 ms o HC-SR04 mede a distância. Abaixo de 50 cm, há presença.
5. Se houver presença, o tempo do vermelho cai pela metade, reduzindo a espera.
6. Dentro do horário de pico (hora obtida por NTP, fuso UTC-3), são usados os tempos de pico.
7. Com status `manutencao`, o LED amarelo pisca e o ciclo normal é suspenso.
8. Se o Wi-Fi ou o MQTT cair, o ESP32 reconecta sozinho sem parar o semáforo.

Exemplo de mensagem aceita no tópico:

```json
{
  "tempoPadrao": { "carro": 25, "pedestre": 25 },
  "tempoHorarioPico": { "carro": 35, "pedestre": 15 },
  "horarioPico": { "inicio": "07:00", "fim": "09:00" },
  "statusOperacional": "ativo"
}
```

## Componentes e pinos

| Componente | Pino do ESP32 | Função |
|---|---|---|
| HC-SR04 (TRIG) | GPIO 5 | Dispara a medição |
| HC-SR04 (ECHO) | GPIO 18 | Recebe o eco |
| LED vermelho | GPIO 25 | Atuador |
| LED amarelo | GPIO 26 | Atuador |
| LED verde | GPIO 27 | Atuador |

Cada LED usa um resistor de 220 Ω em série.

## Arquivos

- `firmware-esp32/sketch.ino`: código principal, comentado com os itens A a F da R5
- `firmware-esp32/diagram.json`: circuito da simulação no Wokwi
- `firmware-esp32/libraries.txt`: bibliotecas (PubSubClient e ArduinoJson)

## Simulação

Projeto no Wokwi: https://wokwi.com/projects/476638328908396

Para testar: rode a simulação, ajuste a distância do HC-SR04 para menos de 50 cm e publique o
JSON de exemplo no tópico `Ifpe/Semaforo/Semaforo1` (por exemplo, pelo HiveMQ Web Client).

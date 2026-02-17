# Smart Lighting System (SLS)

**Faculty of Electrical Engineering** 
**University of Sarajevo  
Department of Telecommunications**

## Technologies Used

- C++17

- Boost.Asio

- OpenSSL (TLS)

- UDP Communication

This project was developed as part of coursework at the Faculty of Electrical Engineering, University of Sarajevo, as an implementation of a secure distributed system with hierarchical control architecture.

---

## Project Description

Smart Lighting System (SLS) is a distributed smart public lighting management system implemented in C++ using Boost.Asio, TLS (OpenSSL).

The system consists of:

- **Central Server** – manages regional servers and system administration
- **Regional Servers** – manage luminaires and receive sensor data
- **Luminaire Clients** – represent smart street lights
- **UDP Sensors** – send environmental and motion data
- **Admin CLI Tool** – provides system state snapshot functionality

## Main Features

- Secure TLS communication between servers
- UDP communication for sensor devices
- Distributed hierarchical architecture
- Real-time system snapshot via administrative interface
- Configuration synchronization between central and regional servers

---

## How to Clone the Project

1. Open the GitHub repository.
2. Click on the **Code** button.
3. Select **HTTPS**.
4. Copy the provided repository URL.
5. Open your terminal and run the following command:

```bash
git clone https://github.com/username/SmartLightingSystem.git

Then enter the project directory:

```bash
cd SmartLightingSystem

## Compilation

All components must be compiled before running from the directory:

```bash
~/asio/SmartLightingSystem

Use the following commands:
```bash
g++ -std=c++17 -O2 -I../include central_server.cpp -lboost_system -lssl -lcrypto -pthread -o central_server
```bash
g++ -std=c++17 -O2 -I../include regional_server.cpp -lboost_system -lssl -lcrypto -pthread -o regional_server
```bash
g++ -std=c++17 -O2 -I../include luminaire_client.cpp -lboost_system -lssl -lcrypto -pthread -o luminaire_client
```bash
g++ -std=c++17 -O2 -I../include sensor_udp.cpp -lboost_system -lssl -lcrypto -pthread -o sensor_udp
```bash
g++ -std=c++17 -O2 -I../include admin_cli.cpp -lboost_system -lssl -lcrypto -pthread -o ad

## Running the System

The Smart Lighting System follows a hierarchical distributed architecture.  
The Central Server must be started first, followed by the Regional Servers, and then the client devices (luminaires and sensors).

Since each component runs as an independent process, the system requires **five separate terminals** to properly simulate the full distributed environment.

Open **five terminal windows** and execute the components in the following order:

---

### Terminal 1 – Central Server

The central server coordinates all regional servers and handles administrative requests.

```bash
./central_server 6000 cert.pem key.pem

### Terminal 2 – Regional Server #1

This regional server manages devices in region 1 and connects securely to the central server.

```bash
./regional_server 1 5555 7777 127.0.0.1 6000 5 cert.pem key.pem 20

### Terminal 3 – Regional Server #2

This regional server manages devices in region 2.

```bash
./regional_server 2 5556 7778 127.0.0.1 6000 5 cert.pem key.pem 20

### Terminal 4 – Luminaire (connected to Region #1)

This process simulates a smart street light connected to regional server #1.

```bash
./luminaire_client 127.0.0.1 5555 sls://lamp-001 1 60 30

### Terminal 5 – UDP Sensor (connected to Region #1)

This process simulates a UDP-based environmental/motion sensor sending data to regional server #1.

```bash
./sensor_udp 127.0.0.1 7777 sls://sensor-001 1 120 25 10 30

### System Snapshot

At any time, you can open an additional terminal (or reuse one after testing) to retrieve a system state snapshot:

```bash
./admin_cli 127.0.0.1 6000

This command connects to the central server and displays the current state of regions, luminaires, and sensors.

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

# Boost.Asio

Boost.Asio is a cross-platform C++ library from the Boost library collection that provides support for network programming and asynchronous I/O operations [1]. It is a widely used and popular library for building high-performance network applications in C++.

## Key Features

- **Asynchronous I/O**  
  Enables non-blocking operations for network and file I/O, making it suitable for high-performance servers and clients.

- **Platform Independence**  
  Abstracts platform-specific differences and provides a consistent API for network programming across operating systems.

- **Thread Safety**  
  Designed to work seamlessly in multithreaded environments for handling concurrent operations.

- **Supported Protocols**
  - TCP
  - UDP
  - Serial ports
  - Timers
  - Local sockets

- **Ease of Use**  
  Combines performance with usability by offering both low-level socket APIs and higher-level abstractions.

## Typical Use Cases

Boost.Asio is commonly used for building:

- HTTP servers
- Chat servers
- Real-time communication systems
- Any application requiring reliable networking capabilities

---

## Installing Boost.Asio

The installation process for Boost.Asio involves the following steps:

### Step 1: Clone the Repository

First, clone the repository to your local system:

```bash
git clone https://github.com/boostorg/asio.git
cd asio
````

---

### Step 2: Initialize Submodules

Boost.Asio uses submodules (e.g., Boost.Config). Initialize and update them with:

```bash
git submodule update --init --recursive
```

---

### Step 3: Install Required Dependencies

Ensure that the following tools are installed:

* A C++ compiler that supports C++11 or newer (GCC, Clang, MSVC)
* CMake (minimum version depends on your platform)
* Build tools (e.g., make, ninja)

**Ubuntu / Debian**

```bash
sudo apt update
sudo apt install build-essential cmake
```

**Fedora / RHEL**

```bash
sudo dnf install gcc-c++ make cmake
```

**macOS**

Install Xcode command line tools:

```bash
xcode-select --install
```

Then install CMake:

```bash
brew install cmake
```

---

## Step 4: Build the Library

Boost.Asio can be built as part of the full Boost distribution or as a standalone library.
For this project and laboratory exercises, the standalone build of Asio is used.

1. Create a build directory:

```bash
mkdir build
cd build
```

2. Run CMake to configure the project:

```bash
cmake ..
```

3. Build the project:

```bash
cmake --build ..
```

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
```

Then enter the project directory:

```bash
cd asio/SmartLightingSystem
```

## Compilation

All components must be compiled before running from the directory:

```bash
~/asio/SmartLightingSystem
```
Use the following commands:
```bash
g++ -std=c++17 -O2 -I../include -I. central_server.cpp db.cpp -lboost_system -lssl -lcrypto -lsqlite3 -pthread -o central_server
```
```bash
g++ -std=c++17 -O2 -I../include -I. regional_server.cpp db.cpp -lboost_system -lssl -lcrypto -lsqlite3 -pthread -o regional_server
```
```bash
g++ -std=c++17 -O2 -I../include -I. luminaire_client.cpp -lboost_system -lssl -lcrypto -pthread -o luminaire_client
```
```bash
g++ -std=c++17 -O2 -I../include sensor_udp.cpp -lboost_system -lssl -lcrypto -pthread -o sensor_udp
```
```bash
g++ -std=c++17 -O2 -I../include -I. admin_cli.cpp -lboost_system -lssl -lcrypto -pthread -o admin_cli
```

## Post-Quantum TLS Key and Certificate Generation

For the TLS-based secure communication in the Smart Lighting System (SLS),
a post-quantum cryptographic (PQC) key pair was generated using OpenSSL with the ML-DSA-44 algorithm.

The following commands were used:
```bash
openssl genpkey -algorithm ml-dsa-44 -out key.pem
openssl req -new -x509 -key key.pem -out cert.pem -days 365 -subj "/CN=sls-pqc"
```
Explanation:
- ml-dsa-44 – Post-Quantum Digital Signature Algorithm (ML-DSA level 44)
- key.pem – Private key used for TLS authentication
- cert.pem – Self-signed X.509 certificate
- x509 – Generates a self-signed certificate
-days 365 – Certificate validity period
- /CN=sls-pqc – Common Name used for TLS identification

The generated certificate and private key are used by the central and regional servers to establish secure TLS connections in accordance with the project requirement for quantum-secure communication (TLS + PQC).

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
```

### Terminal 2 – Regional Server #1

This regional server manages devices in region 1 and connects securely to the central server.

```bash
./regional_server 1 5555 7777 127.0.0.1 6000 5 cert.pem key.pem 20
```

### Terminal 3 – Regional Server #2

This regional server manages devices in region 2.

```bash
./regional_server 2 5556 7778 127.0.0.1 6000 5 cert.pem key.pem 20
```

### Terminal 4 – Luminaire (connected to Region #1)

This process simulates a smart street light connected to regional server #1.

```bash
./luminaire_client 127.0.0.1 5555 sls://lamp-001 1 60 30
```

### Terminal 5 – UDP Sensor (connected to Region #1)

This process simulates a UDP-based environmental/motion sensor sending data to regional server #1.

```bash
./sensor_udp 127.0.0.1 7777 sls://sensor-001 1 120 25 10 30
```

### System Snapshot

At any time, you can open an additional terminal (or reuse one after testing) to retrieve a system state snapshot:

```bash
./admin_cli 127.0.0.1 6000
```

This command connects to the central server and displays the current state of regions, luminaires, and sensors.

---

## Reference

[1] Online. [https://think-async.com/Asio/](https://think-async.com/Asio/)


#!/usr/bin/env bash
set -euo pipefail
valgrind --tool=memcheck --leak-check=full ./central_server 6000 cert.pem key.pem
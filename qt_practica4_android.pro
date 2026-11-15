# Práctica 4 · Diseño de Interfaces 2026B
# Control del robot de la Práctica 2 desde Android (Qt 6) por WebSocket + Base de datos para el sensor DHT22.
# El mismo proyecto compila para escritorio (MinGW / GCC) y para Android (arm64-v8a):
# solo cambia el kit seleccionado en Qt Creator.
# Practica # 4 SQLite en android - NOMBRE - CODIGO - temperatura_XXXX.db

QT += core gui widgets websockets charts sql

CONFIG += c++17

SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h

# Android: Qt genera el AndroidManifest.xml; el permiso INTERNET lo agrega
# el módulo de red (websockets depende de network). Nombre visible de la app:
android {
    ANDROID_TARGET_SDK_VERSION = 35
    ANDROID_VERSION_NAME = 1.0
    ANDROID_VERSION_CODE = 1
}

TARGET = RobotP4

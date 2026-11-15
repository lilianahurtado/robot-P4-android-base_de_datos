#include "mainwindow.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QStyle>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDateTime>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QValueAxis>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

static const int MS_MOVIMIENTO = 400;
static const int MS_REPETIR = 200;
static const int VENTANA_MUESTRAS = 40;
static const char *ALUMNO = "NOMBRE · CODIGO · temperatura_XXXX.db";
static const char *FUENTE = "dht22";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    construirInterfaz();

    // PASO 2: Creación/Apertura de la base de datos SQLite en AppDataLocation
    const QString carpeta = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(carpeta);

    const QString ruta = carpeta + "/temperatura_XXXX.db";
    const bool yaExistia = QFile::exists(ruta);

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(ruta);
    if (!db.open()) {
        registrar("No se abrió la base: " + db.lastError().text());
    } else {
        QSqlQuery consulta;
        if (!consulta.exec("CREATE TABLE IF NOT EXISTS mediciones ("
                           "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                           "fecha_hora TEXT NOT NULL, "
                           "temperatura REAL NOT NULL, "
                           "fuente TEXT NOT NULL, "
                           "nota TEXT)")) {
            registrar("No se creó la tabla: " + consulta.lastError().text());
        } else {
            registrar((yaExistia ? "Base encontrada: " : "Base nueva creada: ") + ruta);

            QSqlQuery pragma("PRAGMA table_info(mediciones)");
            int filasEsquema = 0;
            while (pragma.next()) {
                QString campo = pragma.value(1).toString();
                QString tipo = pragma.value(2).toString();
                registrar(QString("  [Col %1] %2 (%3)").arg(filasEsquema + 1).arg(campo).arg(tipo));
                filasEsquema++;
            }
            registrar(QString("Esquema verificado: %1 campos en la tabla mediciones.").arg(filasEsquema));
        }
    }

    // CONEXIONES Y TIMERS
    connect(&m_socket, &QWebSocket::connected, this, &MainWindow::alConectar);
    connect(&m_socket, &QWebSocket::disconnected, this, &MainWindow::alDesconectar);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &MainWindow::alRecibirMensaje);
    connect(&m_socket, &QWebSocket::errorOccurred, this, &MainWindow::alOcurrirError);

    m_latido.setInterval(1000);
    connect(&m_latido, &QTimer::timeout, this, [this] { enviar({{"tipo", "latido"}}); });

    m_repetir.setInterval(MS_REPETIR);
    connect(&m_repetir, &QTimer::timeout, this, &MainWindow::repetirMovimiento);

    m_servo.setInterval(60);
    m_servo.setSingleShot(true);
    connect(&m_servo, &QTimer::timeout, this, &MainWindow::enviarServoPendiente);

    m_medirAuto.setInterval(500);
    connect(&m_medirAuto, &QTimer::timeout, this, &MainWindow::alPresionarMedir);

    m_barrido.setInterval(700);
    connect(&m_barrido, &QTimer::timeout, this, &MainWindow::siguientePasoBarrido);

    // Temporizador preciso de 10 segundos exactos
    m_dhtAuto.setTimerType(Qt::PreciseTimer);
    m_dhtAuto.setInterval(10000);
    connect(&m_dhtAuto, &QTimer::timeout, this, [this]() {
        enviar({{"tipo", "leer_dht"}});
    });
}

// ---------------------------------------------------------------- interfaz
void MainWindow::construirInterfaz()
{
    auto *central = new QWidget(this);
    auto *raiz = new QVBoxLayout(central);
    raiz->setSpacing(8);

    auto *alumno = new QLabel(QString::fromUtf8(ALUMNO));
    alumno->setAlignment(Qt::AlignCenter);
    alumno->setStyleSheet("font-weight: bold; color: #ffffff;");
    raiz->addWidget(alumno);

    // 1) Conexión
    auto *cajaConexion = new QGroupBox(tr("Conexión con el robot"));
    auto *filaConexion = new QHBoxLayout(cajaConexion);
    m_ip = new QLineEdit(QStringLiteral("192.168.1.101"));
    m_ip->setPlaceholderText(tr("IP de la ESP32-S3"));
    m_ip->setInputMethodHints(Qt::ImhPreferNumbers);
    m_puerto = new QSpinBox;
    m_puerto->setRange(1, 65535);
    m_puerto->setValue(81);
    m_conectar = new QPushButton(tr("Conectar"));
    filaConexion->addWidget(m_ip, 3);
    filaConexion->addWidget(m_puerto, 1);
    filaConexion->addWidget(m_conectar, 2);
    connect(m_conectar, &QPushButton::clicked, this, &MainWindow::alPresionarConectar);
    m_estado = new QLabel(tr("Desconectado"));
    m_estado->setAlignment(Qt::AlignCenter);
    raiz->addWidget(cajaConexion);
    raiz->addWidget(m_estado);

    //empieza aqui
    // 2) Cruceta de movimiento

    auto *cajaMover = new QGroupBox(tr("Movimiento"));
    auto *cruceta = new QGridLayout(cajaMover);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowUp, "avanzar"), 0, 1);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowLeft, "izquierda"), 1, 0);
    cruceta->addWidget(botonMovimiento(QStyle::SP_MediaStop, "parar"), 1, 1);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowRight, "derecha"), 1, 2);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowDown, "retroceder"), 2, 1);



    // Casilla "Matriz" en la esquina inferior izquierda (Fila 2, Columna 0)
    m_chkMatriz = new QCheckBox(tr("Matriz"));
    m_chkMatriz->setChecked(true);
    cruceta->addWidget(m_chkMatriz, 2, 0, Qt::AlignLeft | Qt::AlignVCenter);

    connect(m_chkMatriz, &QCheckBox::toggled, this, [this](bool activo) {
        enviar({{"tipo", "matriz"}, {"estado", activo}});
    });

    raiz->addWidget(cajaMover);

    //desde aqui

    // 3) Servo
    auto *cajaServo = new QGroupBox(tr("Servo del sensor"));
    auto *filaServo = new QHBoxLayout(cajaServo);
    m_slider = new QSlider(Qt::Horizontal);
    m_slider->setRange(0, 180);
    m_slider->setValue(90);
    m_angulo = new QLabel(QStringLiteral("90°"));
    m_angulo->setMinimumWidth(48);
    auto *barrido = new QPushButton(tr("Barrido"));
    filaServo->addWidget(m_slider, 1);
    filaServo->addWidget(m_angulo);
    filaServo->addWidget(barrido);
    connect(m_slider, &QSlider::valueChanged, this, &MainWindow::alMoverServo);
    connect(barrido, &QPushButton::clicked, this, &MainWindow::alPresionarBarrido);
    raiz->addWidget(cajaServo);

    // 4) Distancia: lectura y gráfica
    auto *cajaDist = new QGroupBox(tr("Distancia del HC-SR04"));
    auto *colDist = new QVBoxLayout(cajaDist);
    auto *filaDist = new QHBoxLayout;

    m_distancia = new QLabel(QStringLiteral("— cm"));
    QFont fuenteDistancia = m_distancia->font();
    fuenteDistancia.setPointSize(22);
    fuenteDistancia.setBold(true);
    m_distancia->setFont(fuenteDistancia);

    auto *medir = new QPushButton(tr("Medir"));
    m_auto = new QCheckBox(tr("Automático"));
    filaDist->addWidget(m_distancia, 1);
    filaDist->addWidget(medir);
    filaDist->addWidget(m_auto);
    connect(medir, &QPushButton::clicked, this, &MainWindow::alPresionarMedir);
    connect(m_auto, &QCheckBox::toggled, this, [this](bool activo) {
        activo ? m_medirAuto.start() : m_medirAuto.stop();
    });

    auto *chkAlarma = new QCheckBox(tr("Alarma Sonora"));
    chkAlarma->setChecked(true);
    connect(chkAlarma, &QCheckBox::toggled, this, [this](bool activo) {
        enviar({{"tipo", "alarma"}, {"estado", activo}});
    });
    filaDist->addWidget(chkAlarma);
    colDist->addLayout(filaDist);

    m_serie = new QLineSeries;
    auto *grafica = new QChart;
    grafica->addSeries(m_serie);
    grafica->legend()->hide();
    m_ejeX = new QValueAxis;
    m_ejeX->setRange(0, VENTANA_MUESTRAS);
    m_ejeX->setLabelFormat("%d");
    m_ejeX->setTitleText(tr("muestra"));
    auto *ejeY = new QValueAxis;
    ejeY->setRange(0, 200);
    ejeY->setTitleText(tr("cm"));
    grafica->addAxis(m_ejeX, Qt::AlignBottom);
    grafica->addAxis(ejeY, Qt::AlignLeft);
    m_serie->attachAxis(m_ejeX);
    m_serie->attachAxis(ejeY);
    auto *vista = new QChartView(grafica);
    vista->setRenderHint(QPainter::Antialiasing);
    vista->setMinimumHeight(180);
    colDist->addWidget(vista, 1);
    raiz->addWidget(cajaDist, 1);

    // 5) Registro de mensajes
    m_registro = new QPlainTextEdit;
    m_registro->setReadOnly(true);
    m_registro->setMaximumBlockCount(200);
    m_registro->setMaximumHeight(110);
    raiz->addWidget(m_registro);

    // --- Configuración del QTabWidget (Pestañas) ---
    auto *pestanas = new QTabWidget(this);
    pestanas->addTab(central, "Robot");

    auto *pagTemp = new QWidget;
    m_capaTemp = new QVBoxLayout(pagTemp);
    m_capaTemp->setSpacing(12);
    m_capaTemp->setContentsMargins(15, 15, 15, 15);

    auto *alumnoTemp = new QLabel(QString::fromUtf8(ALUMNO));
    alumnoTemp->setAlignment(Qt::AlignCenter);
    alumnoTemp->setStyleSheet("font-weight: bold; color: #ffffff;");
    m_capaTemp->addWidget(alumnoTemp);

    auto *cajaTemp = new QGroupBox(tr("Sensor de Temperatura DHT22"));
    auto *colTemp = new QVBoxLayout(cajaTemp);

    m_temperatura = new QLabel(tr("-- °C"));
    m_temperatura->setAlignment(Qt::AlignCenter);
    QFont fuenteTemp = m_temperatura->font();
    fuenteTemp.setPointSize(28);
    fuenteTemp.setBold(true);
    m_temperatura->setFont(fuenteTemp);
    m_temperatura->setStyleSheet("color: #bd93f9;");

    auto *botonLeerTemp = new QPushButton(tr("Leer temperatura"));
    botonLeerTemp->setMinimumHeight(45);
    botonLeerTemp->setStyleSheet(
        "QPushButton { background-color: #6272a4; color: white; font-weight: bold; border-radius: 8px; font-size: 14px; }"
        "QPushButton:hover { background-color: #7082b6; }"
        "QPushButton:pressed { background-color: #44475a; }"
        );

    m_chkDhtAuto = new QCheckBox(tr("Guardar cada 10 s"));

    colTemp->addWidget(m_temperatura);
    colTemp->addWidget(botonLeerTemp);
    colTemp->addWidget(m_chkDhtAuto);

    // --- PASO 4: Tabla e Historial ---
    auto *botonCargarHistorial = new QPushButton(tr("Cargar historial"));
    botonCargarHistorial->setStyleSheet(
        "QPushButton { background-color: #bd93f9; color: #1e1e2e; font-weight: bold; border-radius: 6px; padding: 6px; }"
        "QPushButton:hover { background-color: #ff79c6; }"
        );

    m_tabla = new QTableWidget(0, 4);
    m_tabla->setHorizontalHeaderLabels({"id", "fecha y hora", "°C", "nota"});
    m_tabla->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_total = new QLabel(tr("Mediciones guardadas: 0"));
    m_total->setStyleSheet("font-weight: bold; color: #50fa7b;");

    colTemp->addWidget(botonCargarHistorial);
    colTemp->addWidget(m_tabla, 1);
    colTemp->addWidget(m_total);

    // --- PASO 5: Editar Nota (UPDATE) y Eliminar Rango (DELETE) ---
    auto *cajaNota = new QGroupBox(tr("Gestión de registros"));
    auto *colNota = new QVBoxLayout(cajaNota);

    // Fila 1: Editar nota
    auto *filaCamposNota = new QHBoxLayout;
    m_id = new QSpinBox;
    m_id->setRange(0, 99999);
    m_id->setSpecialValueText("-");

    m_nota = new QLineEdit;
    m_nota->setPlaceholderText(tr("Escribe una nota..."));

    auto *botonGuardarNota = new QPushButton(tr("Guardar nota"));
    botonGuardarNota->setStyleSheet(
        "QPushButton { background-color: #50fa7b; color: #1e1e2e; font-weight: bold; border-radius: 6px; padding: 6px; }"
        "QPushButton:hover { background-color: #69ff94; }"
        );

    filaCamposNota->addWidget(new QLabel(tr("ID:")));
    filaCamposNota->addWidget(m_id, 1);
    filaCamposNota->addWidget(m_nota, 3);
    filaCamposNota->addWidget(botonGuardarNota, 2);

    // Fila 2: Eliminar por rango de IDs
    auto *filaEliminarRango = new QHBoxLayout;
    m_idInicio = new QSpinBox;
    m_idInicio->setRange(0, 99999);
    m_idInicio->setSpecialValueText("-");

    m_idFin = new QSpinBox;
    m_idFin->setRange(0, 99999);
    m_idFin->setSpecialValueText("-");

    auto *botonEliminarRango = new QPushButton(tr("Eliminar rango"));
    botonEliminarRango->setStyleSheet(
        "QPushButton { background-color: #ff5555; color: white; font-weight: bold; border-radius: 6px; padding: 6px; }"
        "QPushButton:hover { background-color: #ff6e6e; }"
        );

    filaEliminarRango->addWidget(new QLabel(tr("Desde:")));
    filaEliminarRango->addWidget(m_idInicio, 1);
    filaEliminarRango->addWidget(new QLabel(tr("Hasta:")));
    filaEliminarRango->addWidget(m_idFin, 1);
    filaEliminarRango->addWidget(botonEliminarRango, 2);

    colNota->addLayout(filaCamposNota);
    colNota->addLayout(filaEliminarRango);
    colTemp->addWidget(cajaNota);

    m_capaTemp->addWidget(cajaTemp, 1);

    // Eventos
    connect(botonLeerTemp, &QPushButton::clicked, this, [this]() {
        enviar({{"tipo", "leer_dht"}});
    });

    connect(m_chkDhtAuto, &QCheckBox::toggled, this, [this](bool activo) {
        if (activo) {
            m_dhtAuto.start();
        } else {
            m_dhtAuto.stop();
        }
    });

    connect(botonCargarHistorial, &QPushButton::clicked, this, &MainWindow::cargarHistorial);

    connect(m_tabla, &QTableWidget::cellClicked, this, [this](int fila, int) {
        if (m_tabla->item(fila, 0)) {
            int idTocada = m_tabla->item(fila, 0)->text().toInt();
            m_id->setValue(idTocada);
            m_idInicio->setValue(idTocada);
            m_idFin->setValue(idTocada);
        }
    });

    connect(botonGuardarNota, &QPushButton::clicked, this, &MainWindow::guardarNota);
    connect(botonEliminarRango, &QPushButton::clicked, this, &MainWindow::eliminarRango);

    pestanas->addTab(pagTemp, "Temperatura");
    setCentralWidget(pestanas);

    setWindowTitle(tr("Robot P4 · Android"));

    aplicarEstilosUI();
}

QPushButton *MainWindow::botonMovimiento(QStyle::StandardPixmap icono, const QString &accion)
{
    auto *b = new QPushButton(style()->standardIcon(icono), QString());
    b->setIconSize(QSize(40, 40));
    b->setMinimumSize(72, 64);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    b->setAccessibleName(accion);

    b->setStyleSheet(
        "QPushButton { background-color: #34495e; color: #ecf0f1; border-radius: 12px; border: 2px solid #2c3e50; }"
        "QPushButton:hover { background-color: #415b76; }"
        "QPushButton:pressed { background-color: #2980b9; border: 2px solid #3498db; }"
        );

    connect(b, &QPushButton::pressed, this, [this, accion] {
        if (accion == "parar") {
            m_repetir.stop();
            enviarMover("parar", 0);
            return;
        }
        m_accionActual = accion;
        enviarMover(accion, MS_MOVIMIENTO);
        m_repetir.start();
    });

    connect(b, &QPushButton::released, this, [this, accion] {
        if (accion == "parar") return;
        m_repetir.stop();
        enviarMover("parar", 0);
    });

    return b;
}

void MainWindow::aplicarEstilosUI() {
    this->setStyleSheet(
        "QMainWindow, QTabWidget::pane { background-color: #1e1e2e; border: none; }"
        "QTabBar::tab { background-color: #2b2b3b; color: #a6adc8; padding: 8px 16px; font-weight: bold; border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 2px; }"
        "QTabBar::tab:selected { background-color: #44475a; color: #ffffff; border-bottom: 2px solid #bd93f9; }"
        "QTabBar::tab:hover { background-color: #36364a; color: #ffffff; }"
        "QGroupBox { background-color: #1e1e2e; color: #ffffff; font-weight: bold; border: 1px solid #44475a; border-radius: 8px; margin-top: 10px; padding-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: #ffffff; }"
        "QLabel { color: #ffffff; font-size: 13px; }"
        "QCheckBox { color: #ffffff; font-size: 13px; spacing: 6px; }"
        "QCheckBox::indicator { width: 18px; height: 18px; border-radius: 4px; border: 2px solid #6272a4; background-color: #2b2b3b; }"
        "QCheckBox::indicator:hover { border-color: #bd93f9; }"
        "QCheckBox::indicator:checked { background-color: #ffffff; border: 2px solid #ffffff; image: url(:/qt-project.org/styles/commonstyle/images/check-16.png); }"
        );

    if (m_ip) {
        m_ip->setStyleSheet(
            "QLineEdit { background-color: #2b2b3b; color: #ffffff; border: 2px solid #44475a; "
            "border-radius: 8px; padding: 5px; font-size: 14px; selection-background-color: #bd93f9; }"
            "QLineEdit:focus { border: 2px solid #6272a4; }"
            );
    }

    if (m_conectar) {
        m_conectar->setStyleSheet(
            "QPushButton { background-color: #c0392b; color: white; border-radius: 8px; font-weight: bold; padding: 6px; font-size: 13px; }"
            "QPushButton:hover { background-color: #e74c3c; }"
            );
    }

    if (m_estado) {
        m_estado->setStyleSheet("QLabel { color: #a6adc8; font-weight: bold; }");
    }

    if (m_registro) {
        m_registro->setStyleSheet(
            "QPlainTextEdit { background-color: #181825; color: #a6adc8; border: 1px solid #44475a; border-radius: 6px; font-family: monospace; }"
            );
    }

    if (m_serie && m_serie->chart()) {
        QChart *chart = m_serie->chart();
        chart->setBackgroundBrush(QBrush(QColor("#2b2b3b")));
        chart->setTitleBrush(QBrush(QColor("#ffffff")));
        if (chart->axisX()) {
            chart->axisX()->setLabelsBrush(QBrush(QColor("#ffffff")));
            chart->axisX()->setTitleBrush(QBrush(QColor("#ffffff")));
        }
        if (chart->axisY()) {
            chart->axisY()->setLabelsBrush(QBrush(QColor("#ffffff")));
            chart->axisY()->setTitleBrush(QBrush(QColor("#ffffff")));
        }
    }
}

// ---------------------------------------------------------------- conexión
void MainWindow::alPresionarConectar()
{
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.close();
        return;
    }
    const QUrl url(QStringLiteral("ws://%1:%2").arg(m_ip->text().trimmed()).arg(m_puerto->value()));
    m_estado->setText(tr("Conectando a %1 …").arg(url.toString()));
    m_conectar->setText(tr("Cancelar"));
    m_socket.open(url);
}

void MainWindow::alConectar()
{
    m_estado->setText(tr("Estado: Conectado"));
    m_conectar->setText(tr("Desconectar"));
    m_conectar->setStyleSheet(
        "QPushButton { background-color: #27ae60; color: white; border-radius: 8px; font-weight: bold; padding: 6px; font-size: 13px; }"
        "QPushButton:hover { background-color: #2ecc71; }"
        );
    m_latido.start();
    enviar({{"tipo", "latido"}});
    registrar(tr("conectado"));
}

void MainWindow::alDesconectar()
{
    m_latido.stop();
    m_repetir.stop();
    m_medirAuto.stop();
    m_barrido.stop();
    m_dhtAuto.stop();

    if (m_chkDhtAuto) {
        m_chkDhtAuto->setChecked(false);
    }
    if (m_auto) {
        m_auto->setChecked(false);
    }
    if (m_chkMatriz) {
        m_chkMatriz->setChecked(false);
    }

    m_estado->setText(tr("Estado: Desconectado"));
    m_conectar->setText(tr("Conectar"));
    m_conectar->setStyleSheet(
        "QPushButton { background-color: #c0392b; color: white; border-radius: 8px; font-weight: bold; padding: 6px; font-size: 13px; }"
        "QPushButton:hover { background-color: #e74c3c; }"
        );
    registrar(tr("desconectado"));
}

void MainWindow::alOcurrirError(QAbstractSocket::SocketError)
{
    registrar(tr("error: %1").arg(m_socket.errorString()));
}

void MainWindow::enviar(const QJsonObject &obj)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return;
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void MainWindow::enviarMover(const QString &accion, int ms)
{
    enviar({{"tipo", "mover"}, {"accion", accion}, {"ms", ms}});
}

void MainWindow::repetirMovimiento()
{
    enviarMover(m_accionActual, MS_MOVIMIENTO);
}

// ---------------------------------------------------------------- mensajes
void MainWindow::alRecibirMensaje(const QString &texto)
{
    const QJsonObject obj = QJsonDocument::fromJson(texto.toUtf8()).object();
    const QString tipo = obj.value("tipo").toString();

    if (tipo == "distancia_lectura") {
        agregarDistancia(obj.value("d").toDouble());
    } else if (tipo == "scan") {
        agregarDistancia(obj.value("d").toDouble());
        registrar(tr("scan %1° → %2 cm").arg(obj.value("angulo").toInt())
                      .arg(obj.value("d").toDouble(), 0, 'f', 1));
    } else if (tipo == "dht_lectura") {
        registrar("RX " + texto);
        const QJsonValue t = obj.value("temperatura");
        if (!obj.value("dht_ok").toBool() || !t.isDouble()) {
            registrar("Lectura inválida: no se guarda");
            return;
        }
        if (m_temperatura) {
            m_temperatura->setText(QString::number(t.toDouble(), 'f', 1) + " °C");
        }
        guardarTemperatura(t.toDouble());
    } else if (tipo == "saludo") {
        registrar(tr("saludo de %1").arg(obj.value("nombre_host").toString()));
    } else if (tipo == "mover_err") {
        registrar(tr("el robot rechazó la orden %1").arg(obj.value("accion").toString()));
    } else if (tipo == "latido_ok" || tipo == "telemetria" || tipo.startsWith("mover_")) {
    } else {
        registrar(texto);
    }
}

// ---------------------------------------------------------------- servo y distancia
void MainWindow::alMoverServo(int angulo)
{
    m_angulo->setText(QStringLiteral("%1°").arg(angulo));
    m_anguloPendiente = angulo;
    if (!m_servo.isActive())
        m_servo.start();
}

void MainWindow::enviarServoPendiente()
{
    if (m_anguloPendiente < 0) return;
    enviar({{"tipo", "servo"}, {"angulo", m_anguloPendiente}});
    m_anguloPendiente = -1;
}

void MainWindow::alPresionarMedir()
{
    enviar({{"tipo", "leer_distancia"}});
}

void MainWindow::alPresionarBarrido()
{
    m_anguloBarrido = 0;
    m_barrido.start();
    siguientePasoBarrido();
}

void MainWindow::siguientePasoBarrido()
{
    if (m_anguloBarrido > 180) {
        m_barrido.stop();
        return;
    }
    enviar({{"tipo", "scan"}, {"angulo", m_anguloBarrido}});
    m_slider->blockSignals(true);
    m_slider->setValue(m_anguloBarrido);
    m_slider->blockSignals(false);
    m_angulo->setText(QStringLiteral("%1°").arg(m_anguloBarrido));
    m_anguloBarrido += 30;
}

void MainWindow::agregarDistancia(double cm)
{
    m_distancia->setText(QStringLiteral("%1 cm").arg(cm, 0, 'f', 1));

    if (cm <= 25.0) {
        m_distancia->setStyleSheet("QLabel { font-size: 24px; font-weight: bold; color: #e74c3c; }");
    } else {
        m_distancia->setStyleSheet("QLabel { font-size: 24px; font-weight: bold; color: #2ecc71; }");
    }

    m_serie->append(m_muestra, cm);
    if (m_serie->count() > VENTANA_MUESTRAS)
        m_serie->remove(0);
    ++m_muestra;
    m_ejeX->setRange(qMax(0, m_muestra - VENTANA_MUESTRAS), qMax(VENTANA_MUESTRAS, m_muestra));
}

void MainWindow::registrar(const QString &linea)
{
    m_registro->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss  ") + linea);
}

void MainWindow::guardarTemperatura(double celsius)
{
    const double c1 = qRound(celsius * 10) / 10.0;
    QSqlQuery consulta;
    consulta.prepare("INSERT INTO mediciones (fecha_hora, temperatura, fuente) "
                     "VALUES (:fecha_hora, :temperatura, :fuente)");
    consulta.bindValue(":fecha_hora", QDateTime::currentDateTime().toString(Qt::ISODate));
    consulta.bindValue(":temperatura", c1);
    consulta.bindValue(":fuente", QString::fromUtf8(FUENTE));

    if (!consulta.exec()) {
        registrar("INSERT falló: " + consulta.lastError().text());
        return;
    }

    registrar(QString("Guardada %1 °C con id %2").arg(c1, 0, 'f', 1)
                  .arg(consulta.lastInsertId().toString()));

    cargarHistorial();
}

// ---------------------------------------------------------------- Paso 4: SELECT
void MainWindow::cargarHistorial()
{
    if (!m_tabla || !m_total) return;

    m_tabla->setEditTriggers(QAbstractItemView::NoEditTriggers);

    QSqlQuery consulta("SELECT id, fecha_hora, temperatura, nota FROM mediciones "
                       "ORDER BY id DESC");
    if (!consulta.isActive()) {
        registrar("SELECT falló: " + consulta.lastError().text());
        return;
    }

    m_tabla->setRowCount(0);
    while (consulta.next()) {
        const int fila = m_tabla->rowCount();
        m_tabla->insertRow(fila);
        m_tabla->setItem(fila, 0, new QTableWidgetItem(consulta.value(0).toString()));
        m_tabla->setItem(fila, 1, new QTableWidgetItem(consulta.value(1).toString()));
        m_tabla->setItem(fila, 2, new QTableWidgetItem(
                                      QString::number(consulta.value(2).toDouble(), 'f', 1)));
        m_tabla->setItem(fila, 3, new QTableWidgetItem(consulta.value(3).toString()));
    }
    m_tabla->resizeColumnsToContents();

    QSqlQuery total("SELECT COUNT(*) FROM mediciones");
    if (total.next()) {
        m_total->setText("Mediciones guardadas: " + total.value(0).toString());
    }
}

// ---------------------------------------------------------------- Paso 5: UPDATE
void MainWindow::guardarNota()
{
    if (!m_id || !m_nota) return;

    const int idSeleccionado = m_id->value();

    if (idSeleccionado == 0) {
        registrar("Primero toca una fila");
        return;
    }

    QSqlQuery consulta;
    consulta.prepare("UPDATE mediciones SET nota = :nota WHERE id = :id");
    consulta.bindValue(":nota", m_nota->text().trimmed());
    consulta.bindValue(":id", idSeleccionado);

    if (!consulta.exec()) {
        registrar("UPDATE falló: " + consulta.lastError().text());
        return;
    }

    const int afectadas = consulta.numRowsAffected();
    registrar(QString("Filas cambiadas: %1 (id: %2)").arg(afectadas).arg(idSeleccionado));

    m_nota->clear();
    cargarHistorial();
}

// ---------------------------------------------------------------- Eliminar Rango y Reiniciar Secuencia
void MainWindow::eliminarRango()
{
    if (!m_idInicio || !m_idFin) return;

    int inicio = m_idInicio->value();
    int fin = m_idFin->value();

    if (inicio == 0 || fin == 0) {
        registrar("Selecciona un ID inicial y final válidos");
        return;
    }

    // Si se ingresan invertidos, se ordenan automáticamente
    if (inicio > fin) {
        qSwap(inicio, fin);
    }

    QSqlQuery consulta;
    consulta.prepare("DELETE FROM mediciones WHERE id BETWEEN :inicio AND :fin");
    consulta.bindValue(":inicio", inicio);
    consulta.bindValue(":fin", fin);

    if (!consulta.exec()) {
        registrar("DELETE falló: " + consulta.lastError().text());
        return;
    }

    const int eliminadas = consulta.numRowsAffected();

    // --- REINICIAR CONTADOR DE AUTOINCREMENTO (sqlite_sequence) ---
    QSqlQuery maxQuery("SELECT COALESCE(MAX(id), 0) FROM mediciones");
    if (maxQuery.next()) {
        const int maxIdActual = maxQuery.value(0).toInt();

        QSqlQuery resetSeq;
        resetSeq.prepare("UPDATE sqlite_sequence SET seq = :maxId WHERE name = 'mediciones'");
        resetSeq.bindValue(":maxId", maxIdActual);
        resetSeq.exec();
    }

    registrar(QString("Filas eliminadas: %1 (rango %2 - %3). Contador ajustado.")
                  .arg(eliminadas)
                  .arg(inicio)
                  .arg(fin));

    m_idInicio->setValue(0);
    m_idFin->setValue(0);
    m_id->setValue(0);
    cargarHistorial();
}


#include "MainWindow.h"
#include "ui_MainWindow.h"
#include <QDir>
#include <QCoreApplication>
#include <QPainter>
#include <QFont>
#include <QFontDatabase>
#include <QDateTime>

// ---------- 생성/소멸 ----------
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
    // 1. UI 준비
    ui->setupUi(this);

    // 2. DB 동적 생성
    // db에 이름을 붙이는 이유는 읽기 전용, 쓰기 전용, 다른 db 등 여러 db에 접속할 수 있음을 고려(확장성 고려)
    db = new DbManager("RecognizeConnection");
    // 2.1 DB의 사용자 얼굴 이미지로 학습
    if (trainFromDatabase()) {
        setMessage("모델 준비 완료");
    } else {
        setMessage("모델 준비 실패(등록 데이터 부족 또는 DB 오류)");
    }

    // 3. 시리얼 포트 연결 시도
    if (!openSerial("/dev/rfcomm4", 9600)) {
        if (!openSerial(QString(), 9600)) {
            openSerial("/dev/ttyACM0", 9600);
        }
    }

    // 4. 카메라 시작
    startCamera();
}

MainWindow::~MainWindow() {
    // 1. 카메라 종료
    stopCamera();

    // 2. 시리얼 닫기(MainWindow가 파괴되면서 닫히지만 명시적으로 닫아줌)
    if (serial.isOpen()){
        serial.close();
    }

    // 3. DB 동적 해제
    if (db){
        db->close();
        delete db;
        db = nullptr;
    }

    // 4. UI 제거
    delete ui;
}

// ---------- 변환/유틸 ----------
QImage MainWindow::matToQImage(const cv::Mat& mat) {
    if (mat.empty()) return QImage();
    if (mat.type() == CV_8UC1) {
        return QImage(mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Grayscale8).copy();
    } else if (mat.type() == CV_8UC3) {
        cv::Mat rgb; cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        return QImage(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888).copy();
    } else {
        cv::Mat gray;
        if (mat.channels() == 3) cv::cvtColor(mat, gray, cv::COLOR_BGR2GRAY);
        else mat.convertTo(gray, CV_8U);
        return QImage(gray.data, gray.cols, gray.rows, gray.step, QImage::Format_Grayscale8).copy();
    }
}

void MainWindow::showMatOn(QLabel* label, const cv::Mat& mat, const QString& text) {
    if (!label) return;
    QImage img = matToQImage(mat);
    if (img.isNull()) return;

    if (!text.isEmpty()) {
        QPainter painter(&img);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(Qt::green);

        // 폰트 폴백: Noto → Nanum → 시스템 기본
        QFont font;
        QStringList candidates = {
            "Noto Sans CJK KR", "Noto Sans KR", "NanumGothic", "Nanum Gothic"
        };
        bool set = false;
        for (const QString& fam : candidates) {
            if (QFontDatabase().families().contains(fam)) { font.setFamily(fam); set = true; break; }
        }
        if (!set) {
            // 시스템 기본 사용
            font = painter.font();
        }
        font.setPointSize(14);
        painter.setFont(font);

        painter.drawText(10, 25, text);
        painter.end();
    }

    label->setPixmap(QPixmap::fromImage(img)
                         .scaled(label->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
}


cv::Rect MainWindow::largestRect(const std::vector<cv::Rect>& rects) {
    int idx = -1; int areaMax = -1;
    for (int i=0;i<(int)rects.size();++i) {
        int a = rects[i].area(); if (a > areaMax) { areaMax = a; idx = i; }
    }
    return (idx >= 0) ? rects[idx] : cv::Rect();
}

QString MainWindow::findCascadeLocal() const {
    QStringList candidates;
    candidates << QDir(QCoreApplication::applicationDirPath()).filePath("haarcascade_frontalface_default.xml");
    candidates << QDir::current().filePath("haarcascade_frontalface_default.xml");
    candidates << "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml";
    for (const auto& p : candidates) if (QFile::exists(p)) return p;
    return QString();
}

void MainWindow::ensureCascadeLoaded() {
    if (!cascadePath.isEmpty() && !faceCasc.empty()) return;
    cascadePath = findCascadeLocal();
    if (!cascadePath.isEmpty()) faceCasc.load(cascadePath.toStdString());
}

void MainWindow::setStatus(const QString& s)  {
    if (ui->lblStatus) ui->lblStatus->setText(s);
}

void MainWindow::setMessage(const QString& s) {
    if (ui->lblMessage) ui->lblMessage->setText(s);
}

// 원격 카메라용 설정값 읽기(환경변수 우선, 없으면 기본값)
static QString envOr(const char* key, const QString& fallback = QString()) {
    const QByteArray v = qgetenv(key);
    return v.isEmpty() ? fallback : QString::fromUtf8(v);
}

// ---------- 카메라 ----------
// --- URL(원격) 우선, 실패 시 로컬로 폴백 ---
bool MainWindow::openBestCamera() {
    // 1) 원격 후보들: STREAM_URLS="url1;url2;..."
    QStringList urlCandidates;
    const QString urls = envOr("STREAM_URLS");
    if (!urls.isEmpty()) {
        for (const auto& s : urls.split(';', Qt::SkipEmptyParts))
            urlCandidates << s.trimmed();
    }

    // 이미 열려 있으면 닫기
    if (cap.isOpened()) cap.release();

    // 2) 네트워크 URL 먼저 (FFmpeg → GStreamer 순서)
    for (const QString& url : urlCandidates) {
        if (cap.open(url.toStdString(), cv::CAP_FFMPEG)) {
            setStatus(QString("원격 카메라(FFmpeg) 연결됨: %1").arg(url));
            cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
            return true;
        }
        if (cap.open(url.toStdString(), cv::CAP_GSTREAMER)) {
            setStatus(QString("원격 카메라(GStreamer) 연결됨: %1").arg(url));
            cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
            return true;
        }
    }

    // 3) 로컬 후보: 0→1→2
    for (int idx : {0,1,2}) {
        if (cap.open(idx)) {
            setStatus(QString("로컬 카메라 연결됨 (index=%1)").arg(idx));
            return true;
        }
    }
    setStatus("카메라 열기 실패(원격/로컬 모두)");
    return false;
}


// --- startCamera: URL 우선으로 시작하도록 교체 ---
void MainWindow::startCamera() {
    if (!openBestCamera()) return;           // ← 여기서 cap.open(0) 쓰지 않음
    ensureCascadeLoaded();
    setStatus((cascadePath.isEmpty() || faceCasc.empty())
                  ? "카메라 시작됨 (얼굴 인식 파일 없음)"
                  : "카메라 시작됨 (얼굴 인식 가능)");
    connect(&timer, &QTimer::timeout, this, &MainWindow::onFrameTick, Qt::UniqueConnection);
    timer.start(33);
}


void MainWindow::stopCamera() {
    timer.stop();
    if (cap.isOpened()) cap.release();
    setStatus("카메라 중지됨");
}

// ---------- DB → 학습 ----------
bool MainWindow::trainFromDatabase() {
    // 1. 초기화
    labelToName.clear();
    pairToLabel.clear();
    conflictIds.clear();
    nextLabelId = 1;
#if HAS_OPENCV_FACE
    model = cv::face::LBPHFaceRecognizer::create(1, 8, 8, 8, threshold);
    model->setThreshold(threshold);
#endif

    // 2. DB 연결
    if (!db->open()) {
        setStatus("DB 연결 실패");
        return false;
    }

    // 3. ID - 이름 충돌 스캔(SELECT)
    {
        QHash<int, QSet<QString>> namesPerId;
        QSqlQuery q(db->database());
        if (!q.exec("SELECT user_id, user_name FROM face_images")) {
            setStatus(QString("DB 조회 실패: %1").arg(q.lastError().text()));
            return false;
        }
        while (q.next()) {
            int uid = q.value(0).toInt();
            QString uname = q.value(1).toString().trimmed();
            namesPerId[uid].insert(uname);
        }
        for (auto it = namesPerId.begin(); it != namesPerId.end(); ++it) {
            if (it.value().size() > 1) {
                conflictIds.insert(it.key());
                qWarning() << "[WARN] user_id" << it.key()
                           << "has multiple names:" << it.value().values();
            }
        }
    }

    // 4. 실제 이미지 적재 / 라벨링(SELECT)
    std::vector<cv::Mat> images; // gray 128x128
    std::vector<int> labels;
    int loaded = 0;

    QSqlQuery q2(db->database());
    if (!q2.exec("SELECT user_id, user_name, face_data FROM face_images ORDER BY created_at ASC")) {
        setStatus(QString("DB 조회 실패: %1").arg(q2.lastError().text()));
        return false;
    }

    while (q2.next()) {
        int uid = q2.value(0).toInt();
        QString uname = q2.value(1).toString().trimmed();
        QByteArray ba = q2.value(2).toByteArray();

        // 헬퍼 함수 1
        cv::Mat gray128;
        if (!decodeRowToGray128(ba, gray128)) continue;

        // 헬퍼 함수 2
        int labelInt = -1;
        if (conflictIds.contains(uid)) {
            // ⚠ 충돌하는 user_id는 (uid, uname) 단위로 분리
            labelInt = ensureLabelForPair(uid, uname);
        } else {
            // 충돌 없음: user_id 하나당 단일 이름으로 간주
            // 같은 user_id의 모든 샘플은 같은 labelInt 사용
            labelInt = ensureLabelForPair(uid, uname.isEmpty() ? QString::number(uid) : uname);
        }

        images.push_back(gray128);
        labels.push_back(labelInt);
        loaded++;
    }

    if (loaded == 0) {
        setStatus("DB에 등록 데이터가 없습니다");
        return false;
    }

// 5. 학습 : ON / OFF 분기
#if HAS_OPENCV_FACE
    try {
        model->train(images, labels);
        setStatus(QString("학습 완료: %1장, 클래스 %2개")
                      .arg(loaded)
                      .arg(QSet<int>(labels.begin(), labels.end()).size()));
    } catch (const cv::Exception& e) {
        setStatus(QString("LBPH 학습 오류: %1").arg(e.what()));
        return false;
    }
#else
    trainImages = images;
    trainLabels = labels;
    setStatus(QString("학습(단순 NN) 준비: %1장, 클래스 %2개")
                  .arg(loaded)
                  .arg(QSet<int>(labels.begin(), labels.end()).size()));
#endif
    return true;
}
bool MainWindow::decodeRowToGray128(const QByteArray& png, cv::Mat& outGray128, cv::Mat* outColor128) {
    std::vector<uchar> buf(png.begin(), png.end());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR); // DB에는 컬러 PNG 저장한다고 합의됨
    if (img.empty()) return false;
    cv::Mat gray; cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, outGray128, cv::Size(128,128));
    if (outColor128) {
        cv::Mat color128; cv::resize(img, color128, cv::Size(128,128));
        *outColor128 = color128;
    }
    return true;
}
int MainWindow::ensureLabelForPair(int uid, const QString& uname) {
    QPair<int, QString> key(uid, uname);
    if (!pairToLabel.contains(key)) {
        int label = nextLabelId++;
        pairToLabel.insert(key, label);
        labelToName.insert(label, uname);
    }
    return pairToLabel.value(key);
}

// ---------- 예측 ----------
bool MainWindow::predictLabel(const cv::Mat& roiGray128, int& outLabel, double& outScore) {
#if HAS_OPENCV_FACE
    int label = -1; double conf = 0.0;
    model->predict(roiGray128, label, conf);
    outLabel = label; outScore = conf;
    // LBPH는 낮을수록 유사. conf <= threshold일 때 매칭 성공으로 본다.
    return true;
#else
    // 간단 최근접 이웃 (L2 거리). 낮을수록 유사.
    if (trainImages.empty()) return false;
    double best = 1e18; int bestLabel = -1;
    for (size_t i=0; i<trainImages.size(); ++i) {
        cv::Mat diff; cv::absdiff(roiGray128, trainImages[i], diff);
        diff.convertTo(diff, CV_32F);
        double dist = std::sqrt(cv::sum(diff.mul(diff))[0]);
        if (dist < best) { best = dist; bestLabel = trainLabels[i]; }
    }
    outLabel = bestLabel; outScore = best;
    return true;
#endif
}

// ---------- 프레임 루프 ----------
void MainWindow::onFrameTick() {
    static int emptyCount = 0;

    cv::Mat frame;
    cap >> frame;

    if (frame.empty()) {
        if (++emptyCount >= 15) { // 약 0.5초(33ms*15) 동안 프레임 없으면 재연결
            emptyCount = 0;
            setStatus("카메라 프레임 끊김 → 재연결 시도");
            cap.release();
            openBestCamera(); // 원격 우선, 실패 시 로컬 재시도
        }
        return;
    }
    emptyCount = 0;

    QString overlayText;  // 화면에 덮어쓸 문자열(이름 + 신뢰도/거리)

    // 얼굴 검출
    ensureCascadeLoaded();
    cv::Rect best;
    if (!cascadePath.isEmpty() && !faceCasc.empty()) {
        cv::Mat gray; cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);
        std::vector<cv::Rect> faces;
        faceCasc.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(60,60));
        best = largestRect(faces);
        for (const auto& r : faces) cv::rectangle(frame, r, cv::Scalar(0,255,0), 2);
    }

    // 예측: 얼굴 있으면 ROI 128x128 그레이로 변환하여 모델에 입력
    if (best.area() > 0) {
        cv::Mat roi = frame(best).clone();
        cv::Mat gray; cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
        cv::resize(gray, gray, cv::Size(128,128));

        int label = -1; double score = 0.0;
        if (predictLabel(gray, label, score)) {
#if HAS_OPENCV_FACE
            // LBPH: 낮을수록 유사
            bool ok = (score <= threshold);
            if (ok) {
                QString who = labelToName.value(label, "알 수 없음");
                setMessage(QString("인식: %1 (라벨=%2, 신뢰도=%3)")
                               .arg(who).arg(label).arg(QString::number(score, 'f', 1)));
                overlayText = QString("%1  (신뢰도 %2)").arg(who).arg(QString::number(score, 'f', 1));
            } else {
                setMessage("인식 실패: 미등록");
                overlayText = "미등록";
            }
#else
            // Fallback NN: L2 거리, 낮을수록 유사
            bool ok = (score <= threshold);
            if (ok && label != -1) {
                QString who = labelToName.value(label, "알 수 없음");
                setMessage(QString("인식(NN): %1 (라벨=%2, 거리=%3)")
                               .arg(who).arg(label).arg(QString::number(score, 'f', 1)));
                overlayText = QString("%1  (거리 %2)").arg(who).arg(QString::number(score, 'f', 1));
            } else {
                setMessage("인식(NN) 실패: 미등록");
                overlayText = "미등록";
            }
#endif
            sendSerial(ok);
        } else {
            setMessage("예측 실패");
            overlayText = "예측 실패";
            sendSerial(false);
        }
    } else {
        overlayText = "얼굴을 화면 중앙에 맞춰주세요";
        setMessage(overlayText);
        sendSerial(false);
    }

    // ✅ 한글 오버레이는 showMatOn에서 QPainter로 그립니다.
    showMatOn(ui->videoLabel, frame, overlayText);
}

// 블루투스(rfcomm) 우선 포트 자동 탐색
static QString pickBtPort() {
#ifdef Q_OS_LINUX
    // 1순위: 고정 바인딩된 rfcomm4
    if (QFile::exists("/dev/rfcomm4")) return "/dev/rfcomm4";
#endif
    // 2순위: rfcomm 계열/블루투스 장치 추정
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto& info : ports) {
        const QString name = info.systemLocation();   // /dev/rfcomm1, /dev/ttyACM0 등
        const QString desc = info.description();      // "Bluetooth Device", "HC-06" …
        const QString manf = info.manufacturer();
        if (name.contains("rfcomm")
            || desc.contains("HC-06", Qt::CaseInsensitive)
            || desc.contains("BT04", Qt::CaseInsensitive)
            || manf.contains("Bluetooth", Qt::CaseInsensitive)) {
            return name;
        }
    }
    // 3순위: 기존 USB(개발용) 포트
    for (const auto& info : ports) {
        const QString name = info.systemLocation();
        if (name.contains("ttyACM") || name.contains("ttyUSB"))
            return name;
    }
    return {};
}
// ----- 시리얼 열기 -----
bool MainWindow::openSerial(const QString& portName, int baud) {
    // 1. OPEN 여부 확인(이미 오픈되어 있으면 성공 반환)
    if (serial.isOpen()) return true;

    // 2. 아닌 경우 portName 저장
    QString port = portName;

    // 3. portName이 ""(없음)인 경우 포트 자동선택으로 분기
    // 헬퍼함수 1 / pickBtPort()
#ifdef Q_OS_LINUX
    if (port.isEmpty() || (port.startsWith("/dev/") && !QFile::exists(port))) {
        port = pickBtPort();
    }
#else
    if (port.isEmpty()) port = pickBtPort();
#endif
    if (port.isEmpty()) {
        setStatus("사용 가능한 시리얼 포트를 찾지 못했습니다. (/dev/rfcomm4 바인딩 확인)");
        return false;
    }

    // 4. 시리얼 파라미터 설정
    serial.setPortName(port);
    serial.setBaudRate(baud);                 // HC-06 기본 9600
    serial.setDataBits(QSerialPort::Data8);
    serial.setParity(QSerialPort::NoParity);
    serial.setStopBits(QSerialPort::OneStop);
    serial.setFlowControl(QSerialPort::NoFlowControl);

    // 5. 시리얼 열기
    if (!serial.open(QIODevice::ReadWrite)) {
        setStatus(QString("시리얼 열기 실패: %1 (%2)")
                      .arg(port).arg(serial.errorString()));
        return false;
    }

    // 5.1 에러 발생 시 자동 재연결(0.5초 후 재시도)
    connect(&serial, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError e) {
                if (e == QSerialPort::ResourceError || e == QSerialPort::PermissionError) {
                    serial.close();
                    QTimer::singleShot(500, [this]() {
                        // 마지막 설정 유지해 재시도 (포트 자동 선택)
                        openSerial(QString(), serial.baudRate());
                    });
                }
            });

    // 6. 성공 메시지 설정
    setStatus(QString("시리얼 연결됨: %1 @ %2bps").arg(port).arg(baud));
    return true;
}

// ----- 제어 신호 보내기 -----
void MainWindow::sendSerial(bool on) {
    if (!serial.isOpen()) {
        if (!openSerial(QString(), 9600)) return;
    }

    // 튜닝 파라미터
    static const int OPEN_CONFIRM_FRAMES = 5;     // 연속 ok 프레임 수(≈ 5*33ms ≈ 165ms)
    static const int CLOSE_GRACE_MS      = 3000;  // ok 끊긴 뒤 닫기까지 대기 시간

    // 내부 상태
    /* removed local static isOpen; using MainWindow::isOpen */
    // 현재 문이 "열림 상태"라고 판단했는가
    static int    okStreak = 0;           // 연속 ok 카운트
    static qint64 lastSeenOkMs = 0;       // 마지막으로 ok 본 시각(ms)

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 연속 ok 카운트/타임스탬프 갱신
    if (on) {
        okStreak++;
        lastSeenOkMs = now;
    } else {
        okStreak = 0;
    }

    // 1) 아직 안 열린 상태에서, ok가 충분히 안정되면 한 번만 OPEN 전송
    if (!isOpen && okStreak >= OPEN_CONFIRM_FRAMES) {
        serial.write("OPEN\n");
        serial.flush();
        serial.waitForBytesWritten(10);
        isOpen = true;
        // OPEN 직후에는 바로 CLOSE가 나가지 않도록 lastSeenOkMs가 now로 찍혀 있음
        return;
    }

    // 2) 열린 상태에서, ok가 일정 시간(CLOSE_GRACE_MS) 동안 안 보이면 한 번만 CLOSE 전송
    if (isOpen && (now - lastSeenOkMs) > CLOSE_GRACE_MS) {
        serial.write("CLOSE\n");
        serial.flush();
        serial.waitForBytesWritten(10);
        isOpen = false;
        return;
    }

    // 그 외에는 아무 것도 보내지 않음 (스팸 방지)
}


void MainWindow::handleSerialLine(const QByteArray& raw)
{
    const QString s = QString::fromUtf8(raw).trimmed();
    if (s.isEmpty()) return;

    if (s.startsWith("DONE: OPEN")) {
        this->isOpen = true;
        return;
    }
    if (s.startsWith("DONE: CLOSE")) {
        this->isOpen = false;
        return;
    }
    if (s.startsWith("AUTO: CLOSE")) {
        this->isOpen = false;
        return;
    }
    if (s.startsWith("SENSOR: CLOSED")) {
        this->lastReedClosed = true;
        return;
    }
    if (s.startsWith("SENSOR: OPENED")) {
        this->lastReedClosed = false;
        return;
    }
    if (s.startsWith("READY")) {
        // request sync on boot
        serial.write("STATUS?\n");
        return;
    }
    if (s == "LOCKED") {
        this->isOpen = false;
        return;
    }
    if (s == "UNLOCKED") {
        this->isOpen = true;
        return;
    }
}

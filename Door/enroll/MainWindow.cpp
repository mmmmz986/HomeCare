#include "MainWindow.h"
#include "ui_MainWindow.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QDir>
#include <QPixmap>
#include <QImage>
#include <QCoreApplication>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

// ---------- 경로 탐색: Haar Cascade ----------
static QString findCascadeLocal() {
    QStringList candidates;
    // 실행 파일 폴더, 현재 작업 폴더
    candidates << QDir(QCoreApplication::applicationDirPath()).filePath("haarcascade_frontalface_default.xml");
    candidates << QDir::current().filePath("haarcascade_frontalface_default.xml");
    // 우분투 기본 경로
    candidates << "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml";
    for (const auto& p : candidates) if (QFile::exists(p)) return p;
    return QString();
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

cv::Rect MainWindow::largestRect(const std::vector<cv::Rect>& rects) {
    int idx = -1; int areaMax = -1;
    for (int i=0;i<(int)rects.size();++i) {
        int a = rects[i].area(); if (a > areaMax) { areaMax = a; idx = i; }
    }
    return (idx >= 0) ? rects[idx] : cv::Rect();
}

void MainWindow::showMatOn(QLabel* label, const cv::Mat& mat) {
    if (!label) return;
    QImage img = matToQImage(mat);
    if (img.isNull()) return;
    label->setPixmap(QPixmap::fromImage(img)
                         .scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void MainWindow::ensureCascadeLoaded() {
    if (!cascadePath.isEmpty() && !faceCasc.empty()) return;
    cascadePath = findCascadeLocal();
    if (!cascadePath.isEmpty()) faceCasc.load(cascadePath.toStdString());
}

void MainWindow::setStatus(const QString& s)  {
    if (ui->lblStatus)  ui->lblStatus->setText(s);
    // statusBar()->showMessage(s); // 중복 표시 원치 않으면 비활성
}

void MainWindow::setMessage(const QString& s) {
    if (ui->lblMessage) ui->lblMessage->setText(s);
}

// ---------- 카메라 ----------
void MainWindow::startCamera() {
    if (!cap.isOpened()) {
        if (!cap.open(0)) {
            setStatus("카메라 열기 실패");
            QMessageBox::critical(this, "카메라 오류", "카메라를 열 수 없습니다.");
            return;
        }
    }
    ensureCascadeLoaded();
    setStatus((cascadePath.isEmpty() || faceCasc.empty())
                  ? "카메라 시작 (얼굴 인식 불가)"
                  : "카메라 시작 (얼굴 인식 가능)");
    connect(&timer, &QTimer::timeout, this, &MainWindow::onFrameTick, Qt::UniqueConnection);
    timer.start(33); // ~30fps
}

void MainWindow::stopCamera() {
    timer.stop();
    if (cap.isOpened()) cap.release();
    setStatus("카메라 중지됨");
}

// ---------- 생성자/소멸자 ----------
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
    ui->setupUi(this);
    connect(ui->btnEnroll, &QPushButton::clicked, this, &MainWindow::onCaptureAndEnroll);
    if (ui->videoLabel) ui->videoLabel->setText("Video Preview");
    setMessage("인증용 사진 저장 준비완료");
    startCamera(); // 앱 시작 즉시 카메라 켬
}

MainWindow::~MainWindow() {
    stopCamera();
    db.close();
    delete ui;
}

// ---------- DB INSERT (컬러 PNG 저장) ----------
bool MainWindow::insertFacePng(int userId, const QString& userName, const cv::Mat& color128) {
    std::vector<uchar> buf;
    cv::imencode(".png", color128, buf);
    QByteArray ba(reinterpret_cast<const char*>(buf.data()), (int)buf.size());

    if (!db.open()) { setMessage("DB 연결 실패 (설정/환경 변수 확인)"); return false; }
    QSqlQuery q(db.database());
    q.prepare("INSERT INTO face_images (user_id, user_name, face_data) VALUES (:uid, :uname, :data)");
    q.bindValue(":uid",   userId);
    q.bindValue(":uname", userName);
    q.bindValue(":data",  ba);
    if (!q.exec()) {
        setMessage(QString("DB 저장 오류: %1").arg(q.lastError().text()));
        return false;
    }
    return true;
}

// ---------- 등록 버튼 ----------
void MainWindow::onCaptureAndEnroll() {
    bool ok=false;

    int userId = QInputDialog::getInt(this, "등록", "사용자 ID(숫자):", 1, 1, 1000000, 1, &ok);
    if (!ok) return;

    QString userName = QInputDialog::getText(this, "등록", "사용자 이름:", QLineEdit::Normal, "", &ok);
    if (!ok || userName.trimmed().isEmpty()) return;

    capturing = true;
    capturedCount = 0;
    currentUserId = userId;
    currentUserName = userName.trimmed();
    captureClock.restart();
    setMessage(QString("사용자 %1(%2) 등록 중... (0/%3)")
                   .arg(currentUserName).arg(currentUserId).arg(targetCount));
}

// ---------- 타이머 ----------
void MainWindow::onFrameTick() {
    cv::Mat frame;
    cap >> frame;
    if (frame.empty()) return;

    // 라이브 프리뷰 + 얼굴 박스(표시용)
    ensureCascadeLoaded();
    cv::Rect faceR;
    if (!cascadePath.isEmpty() && !faceCasc.empty()) {
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);
        std::vector<cv::Rect> faces;
        faceCasc.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(60,60));
        for (const auto& r : faces)
            cv::rectangle(frame, r, cv::Scalar(0,255,0), 2);
        faceR = largestRect(faces);
    }
    showMatOn(ui->videoLabel, frame);

    // 캡처 중이 아니라면 저장X
    // 또한 일정 간격으로 저장하기 위한 안전장치
    if (!capturing) return;
    if (captureClock.elapsed() < minIntervalMs) return;
    // 저장용 프레임 (조금 더 안정적인 캡처)
    cv::Mat shot;
    cap >> shot;
    if (shot.empty()) return;
    // 저장용 컬러 ROI
    cv::Mat gray;
    cv::cvtColor(shot, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);
    cv::Rect r;
    if (!cascadePath.isEmpty() && !faceCasc.empty()) {
        std::vector<cv::Rect> faces;
        faceCasc.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(60,60));
        r = largestRect(faces);
    }
    // 사이즈 변경(128x128)
    cv::Mat color128;
    if (r.area() > 0) { // 얼굴 있는 경우
        cv::Mat roiColor = shot(r).clone();
        cv::resize(roiColor, color128, cv::Size(128,128));
    } else {
        // 예외처리 중앙 컬러 크롭
        int w = shot.cols, h = shot.rows;

        int sz = std::min(std::min(w, h), 256);
        if (sz < 128) sz = std::min(w, h);

        int cx = std::max(0, w/2 - sz/2), cy = std::max(0, h/2 - sz/2);

        cv::Rect center(cv::Point(cx, cy), cv::Size(std::min(sz, w - cx), std::min(sz, h - cy)));
        cv::Mat roiColor = shot(center).clone();
        cv::resize(roiColor, color128, cv::Size(128,128));
    }
    if (insertFacePng(currentUserId, currentUserName, color128)) {
        capturedCount++;
        setMessage(QString("사용자 %1(%2) 등록 중... (%3/%4)")
                       .arg(currentUserName).arg(currentUserId).arg(capturedCount).arg(targetCount));
        captureClock.restart();
    }

    if (capturedCount >= targetCount) {
        capturing = false;
        setMessage(QString("등록 완료: %1(%2), %3장 저장완료")
                       .arg(currentUserName).arg(currentUserId).arg(capturedCount));
        QMessageBox::information(this, "등록", "등록완료 되었습니다.");
    }
}

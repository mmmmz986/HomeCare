#pragma once
#include <QMainWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QLabel>
#include <opencv2/opencv.hpp>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QDebug>

class DbManager {
public:
    DbManager(const QString& connectionName = "EnrollConnection") {
        db = QSqlDatabase::addDatabase("QMYSQL", connectionName);
        db.setHostName("127.0.0.1");
        db.setPort(3306);
        db.setDatabaseName("enroll_recognize");
        db.setUserName("root");
        db.setPassword("Marin0806!");
        db.setConnectOptions("MYSQL_OPT_RECONNECT=1;CLIENT_FOUND_ROWS=1;");

        if (!db.open()) {
            qCritical() << "Database connection failed:" << db.lastError().text();
        } else {
            qDebug() << "Database connection established.";
        }
    }

    
    bool open() {
        if (!db.isValid()) return false;
        if (db.isOpen()) return true;
        if (!db.open()) {
            qCritical() << "Database open() failed:" << db.lastError().text();
            return false;
        }
        return true;
    }

    void close() {
        if (!db.isValid()) return;
        if (db.isOpen()) db.close();
        // Detach handle and remove by connection name if available
        QString name = db.connectionName();
        db = QSqlDatabase();
        if (!name.isEmpty()) QSqlDatabase::removeDatabase(name);
        qDebug() << "Database connection closed.";
    }

    bool isOpen() const {
        return db.isValid() && db.isOpen();
    }

    QSqlDatabase database() const { return db; }

private:
    QSqlDatabase db;
};

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onFrameTick();
    void onCaptureAndEnroll(); // 누르면 15장 자동 캡처 시작

private:
    Ui::MainWindow *ui;
    DbManager db{"EnrollConnection"};
    QTimer timer;
    cv::VideoCapture cap;

    // 캡처 세션 상태
    bool capturing = false;
    int  targetCount = 15;        // 한 번 등록 시 저장할 장수
    int  capturedCount = 0;       // 현재까지 저장한 장수
    int  minIntervalMs = 150;     // 캡처 최소 간격 (ms)
    QElapsedTimer captureClock;
    int currentUserId = -1;
    QString currentUserName;

    // 얼굴 검출
    cv::CascadeClassifier faceCasc;
    QString cascadePath;

    // helpers
    void showMatOn(QLabel* label, const cv::Mat& mat);
    static QImage matToQImage(const cv::Mat& mat);
    static cv::Rect largestRect(const std::vector<cv::Rect>& rects);
    void ensureCascadeLoaded();
    bool insertFacePng(int userId, const QString& userName, const cv::Mat& color128);
    void startCamera();  // 앱 시작 시 자동 실행
    void stopCamera();
    void setStatus(const QString& s);   // 한글 상태 메시지
    void setMessage(const QString& s);  // 한글 진행/완료 메시지
};

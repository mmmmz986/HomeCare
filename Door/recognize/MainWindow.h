#pragma once
#include <QMainWindow>
#include <QTimer>
#include <QLabel>
#include <QString>
#include <QElapsedTimer>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QHash>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QFile>
#include <QSet>
#include <QPair>
#include <opencv2/opencv.hpp>
#include "DbManager.h"


#if __has_include(<opencv2/face.hpp>)
#include <opencv2/face.hpp>
#define HAS_OPENCV_FACE 1
#else
#define HAS_OPENCV_FACE 0
#endif


// QPair<int, QString>형 key
inline uint qHash(const QPair<int, QString>& key, uint seed = 0) {
    return qHash(key.first, seed) ^ qHash(key.second, seed * 1315423911u);
}

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    void handleSerialLine(const QByteArray& raw);

private slots:
    void onFrameTick();

private:
    bool isOpen = false;
    bool lastReedClosed = false; // cached SENSOR state (true=CLOSED)
    Ui::MainWindow *ui;
    DbManager* db = nullptr;
    QTimer timer;
    cv::VideoCapture cap;
    QSerialPort serial;

    // 얼굴 검출
    cv::CascadeClassifier faceCasc;
    QString cascadePath;
    // 예측라벨(int) -> 표시이름(String)
    QHash<int, QString> labelToName;
    // (user_id, user_name) -> 내부 정수 라벨
    QHash<QPair<int, QString>, int> pairToLabel;
    int nextLabelId = 1;
    // 충돌 감지: 특정 user_id에 복수 이름이 있으면 true
    QSet<int> conflictIds;
    // 유틸: (uid, uname) 쌍을 정수 라벨로 매핑
    int ensureLabelForPair(int uid, const QString& uname);
    // 장치 식별자(서버팀과 합의한 값으로 바꾸면 됨)
    const QString kDeviceId = "front-door-01";


#if HAS_OPENCV_FACE
    // LBPH
    cv::Ptr<cv::face::LBPHFaceRecognizer> model;
    double threshold = 75.0;   // LBPH: 낮을수록 더 유사. 환경에 맞춰 조정.
#else
    // Fallback: 간단 최근접 이웃 (L2) — 품질은 낮지만 의존성 없이 동작
    std::vector<cv::Mat> trainImages;  // gray 128x128, CV_8U
    std::vector<int>     trainLabels;
    double threshold = 3000.0; // L2 거리 임계값(환경에 맞춰 조정)
#endif
    // 카메라
    bool openBestCamera();
    void startCamera();
    void stopCamera();
    // 상태표시
    void setStatus(const QString& s);
    void setMessage(const QString& s);
    void showMatOn(QLabel* label, const cv::Mat& mat, const QString& text = QString());
    static QImage matToQImage(const cv::Mat& mat);
    // HaarCascade
    static cv::Rect largestRect(const std::vector<cv::Rect>& rects);    
    void ensureCascadeLoaded();
    QString findCascadeLocal() const;
    // LBPH 학습/예측
    bool trainFromDatabase();                 // DB → 이미지/라벨 로드 → 학습
    bool decodeRowToGray128(const QByteArray& png, cv::Mat& outGray128, cv::Mat* outColor128=nullptr);
    bool predictLabel(const cv::Mat& roiGray128, int& outLabel, double& outScore);
    // 아두이노 통신
    bool openSerial(const QString& portName = QString("/dev/ttyACM0"), int baud = 9600);
    void sendSerial(bool on);   // true=LED ON, false=LED OFF
};

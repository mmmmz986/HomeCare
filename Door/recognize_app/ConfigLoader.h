#pragma once
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QCoreApplication>

struct DbConfig {
    QString driver;
    QString host;
    int     port;
    QString database;
    QString user;
    QString password;
    QString charset;
};

// Loads DB config from db_config.json in the executable's directory.
// Environment variables override file values: DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASSWORD, DB_CHARSET
class ConfigLoader {
public:
    static DbConfig load() {
        DbConfig cfg;
        cfg.driver   = "QMYSQL";
        cfg.host     = "127.0.0.1";
        cfg.port     = 3306;
        cfg.database = "enroll_recognize";
        cfg.user     = "root";
        cfg.password = "Marin0806!";
        cfg.charset  = "utf8mb4";
        return cfg;
    }
};

#pragma once
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QDebug>

#define DB_DRIVER "QMYSQL"
#define DB_HOST "127.0.0.1"
#define DB_PORT 3306
#define DB_NAME "enroll_recognize"
#define DB_USER "root"
#define DB_PASS "Marin0806!"

class DbManager {
public:
    explicit DbManager(const QString& connectionName)
        : connName(connectionName) {}

    bool open() {
        if (QSqlDatabase::contains(connName)) {
            db = QSqlDatabase::database(connName);
        } else {
            db = QSqlDatabase::addDatabase(DB_DRIVER, connName);
            db.setHostName(DB_HOST);
            db.setPort(DB_PORT);
            db.setDatabaseName(DB_NAME);
            db.setUserName(DB_USER);
            db.setPassword(DB_PASS);
        }
        if (!db.open()) {
            qCritical() << "[DbManager] Failed to open DB:" << db.lastError().text();
            return false;
        }
        QSqlQuery q(db);
        q.exec("SET NAMES utf8mb4");
        return true;
    }

    void close() {
        if (db.isValid()) {
            db.close();
        }
        QSqlDatabase::removeDatabase(connName);
    }

    QSqlDatabase database() const { return db; }

private:
    QString connName;
    QSqlDatabase db;
};

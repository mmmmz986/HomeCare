#pragma once
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QDebug>
#include "ConfigLoader.h"

class DbManager {
public:
    explicit DbManager(const QString& connectionName)
        : connName(connectionName) {}

    bool open() {
        if (QSqlDatabase::contains(connName)) {
            db = QSqlDatabase::database(connName);
        } else {
            DbConfig cfg = ConfigLoader::load();
            db = QSqlDatabase::addDatabase(cfg.driver, connName);
            db.setHostName(cfg.host);
            db.setPort(cfg.port);
            db.setDatabaseName(cfg.database);
            db.setUserName(cfg.user);
            db.setPassword(cfg.password);
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

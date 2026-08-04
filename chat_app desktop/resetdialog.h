#ifndef RESETDIALOG_H
#define RESETDIALOG_H

#include <QDialog>
#include "global.h"
#include <QMap>

namespace Ui {
class ResetDialog;
}

class ResetDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ResetDialog(QWidget *parent = nullptr);
    ~ResetDialog();

private:
    Ui::ResetDialog *ui;

    void showtip(QString str,QString state);

    bool checkPassValid();

    void initHttpHandlers();

    QMap<ReqId,std::function<void(const QJsonObject&)>> _handlers;

signals:
    void switchLogin();

private slots:
    void on_return_btn_clicked();

    void on_reset_btn_clicked();

    void slot_reset_mod_finish(ReqId id, QString res, ErrorCodes err);

    void on_get_code_clicked();

};

#endif // RESETDIALOG_H

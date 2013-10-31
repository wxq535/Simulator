#ifndef TESTWINDOW_H
#define TESTWINDOW_H

#include <QWidget>
#include <QList>
#include "roadbranchwidget.h"
#include "win_qextserialport.h"

class QTextEdit;
class QTextBrowser;
class QPushButton;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QLabel;

class QTimer;

class TestWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TestWindow(QWidget *parent = 0);
    ~TestWindow();

signals:
    void laneIndexSignal(int index, int color);
    void closeLightSignal();
    void enableLaneIdCmbSignal(bool);

public slots:
    void startSimulatorToggledSlot(bool);
    void sendMsgTimerTimeOutSlot();
    void timerTimeOutSlot();
    void openSerialTriggeredSlot(bool);

private:
    void closeEvent(QCloseEvent *);

private:
    void initPage();
    void initSignalSlots();
    bool checkLaneId();
    void packComData(int lane_index);

    void dumpComData();
    void test();

private:
    QByteArray com_array_;
    int curr_lane_index_;
    int curr_lane_id_;
    bool serial_status_;

    QTimer *send_msg_timer_;
    QTimer *timer_;

    bool need_leave_;

    QList<int> lane_id_list_;

private:
    QLineEdit *port_lineedit_, *baud_rate_lineedit_, *data_bit_lineedit_, *stop_lineedit_, *parity_lineedit_;
    QTextEdit *txt_edit_;
    QSpinBox *timespan_spinbox_;
    QPushButton *open_close_button_, *start_button_;
    QLabel *open_tip_label_;

    Win_QextSerialPort *my_com_;
    RoadBranchWidget *road_branch_widget_;

};

#endif // TESTWINDOW_H
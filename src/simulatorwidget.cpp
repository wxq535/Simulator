#include "simulatorwidget.h"
#include "macrostrings.h"
#include "serialdata.h"
#include "mutility.h"
#include "detectorideditwidget.h"

#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QGroupBox>
#include <QSpinBox>
#include <QLabel>
#include <QLCDNumber>
#include <QMessageBox>
#include <QTime>
#include <QDateTime>
#include <QTimer>
#include <QDebug>

SimulatorWidget::SimulatorWidget(QWidget *parent) :
    QWidget(parent)
{
    struct PortSettings my_com_setting_ = {BAUD9600, DATA_8, PAR_NONE, STOP_1, FLOW_OFF, 500};
    my_com_ = new Win_QextSerialPort("com1", my_com_setting_, QextSerialBase::EventDriven);

    detector_edit_widget_ = new DetectorIdEditWidget(this);

    curr_lane_index_ = 0;
    curr_lane_id_ = 0;
    serial_status_ = false;
    send_msg_timer_ = new QTimer(this);
    timer_ = new QTimer(this);
    need_leave_ = false;

    setWindowTitle(STRING_UI_WINDOW_TITLE);
    initPage();
    initSignalSlots();
    setFixedSize(824+170,606);
    start_button_->setEnabled(false);
}

SimulatorWidget::~SimulatorWidget()
{
    if (timer_ != NULL)
    {
        delete timer_;
        timer_ = NULL;
    }
}

void SimulatorWidget::startSimulatorToggledSlot(bool checked)
{
    if (!checkLaneId())
    {
        start_button_->setChecked(!checked);
        return;
    }
    initMyComSetting();
    if (checked)
    {
        emit enableLaneIdCmbSignal(false);
        start_button_->setText(STRING_UI_STOP);
        int secs = timespan_spinbox_->value();
        send_msg_timer_->start(secs*1000);

        QTime t = QTime::currentTime();
        qsrand(t.msec() + t.second()*1000);
        curr_lane_index_ = qrand() % 12;
        emit laneIndexSignal(curr_lane_index_, RoadBranchWidget::Green);
        packComData(curr_lane_index_);
        my_com_->write(com_array_);
        if (need_leave_)
        {
            timer_->start(qrand() % 1000 + 1000);
        }
        txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
    }
    else
    {
        start_button_->setText(STRING_UI_START);
        send_msg_timer_->stop();
        timer_->stop();
        enableComSetting(true);
        emit enableLaneIdCmbSignal(true);
        road_branch_widget_->closeLightSlot();
    }
}

void SimulatorWidget::sendMsgTimerTimeOutSlot()
{
    if (timer_->isActive())
    {
        return;
    }
    qDebug() << "send msg time out";
    startSimulatorToggledSlot(true);
}

void SimulatorWidget::timerTimeOutSlot()
{
    timer_->stop();
    com_array_[1] = 0x02 + '\0';
    char ms[4] = {'\0'};
    int secs = QDateTime::currentDateTime().toTime_t();
    memcpy(ms, &secs, sizeof(secs));
    com_array_[3] = ms[0];
    com_array_[4] = ms[1];
    txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
    emit laneIndexSignal(curr_lane_index_, RoadBranchWidget::Red);
    int sz = my_com_->write(com_array_);
}

void SimulatorWidget::openSerialTriggeredSlot(bool checked)
{
    if (checked)
    {
        enableComSetting(false);
        open_close_button_->setText(STRING_UI_CLOSE + STRING_UI_SERIALPORT);
        bool status = my_com_->open(QIODevice::ReadWrite);
        serial_status_ = status;
        if (!status)
        {
            QMessageBox::information(this, STRING_TIP, STRING_UI_SERIALPORT + STRING_UI_OPEN + STRING_FAILED, STRING_OK);
            open_close_button_->setChecked(false);
            return;
        }
        else
        {
            open_tip_label_->setText("<font color=\"Red\">OK</font>");
            start_button_->setEnabled(true);
        }
    }
    else
    {
        enableComSetting(true);
        open_close_button_->setText(STRING_UI_OPEN + STRING_UI_SERIALPORT);
        if (start_button_->isChecked())
        {
            startSimulatorToggledSlot(false);
        }
        my_com_->close();
        open_tip_label_->clear();
        start_button_->setEnabled(false);
    }
}

void SimulatorWidget::detectorEditButtonClicked()
{
    detector_edit_widget_->setWindowModality(Qt::ApplicationModal);
    detector_edit_widget_->show();
}

void SimulatorWidget::connectButtonClicked()
{

}

void SimulatorWidget::closeEvent(QCloseEvent *)
{
    if (timer_->isActive())
    {
        timer_->stop();
    }
    if (send_msg_timer_->isActive())
    {
        send_msg_timer_->stop();
    }
}

void SimulatorWidget::initPage()
{
    initComSettingLayout();
    initRoadbranchLayout();
    initScheduleInfoLayout();

    QHBoxLayout *hlayout = new QHBoxLayout;
    hlayout->addWidget(com_setting_grp_, 0);
    hlayout->addWidget(roadbranch_grp_, 1);
    hlayout->addWidget(schedule_grp_, 0);
    setLayout(hlayout);

    QPalette pal;
    pal.setColor(QPalette::Background, QColor(233, 246, 254));
    setPalette(pal);
    QString qss = "QLineEdit{background-color:rgb(233,246,254);}"
            "QGroupBox{border:1px solid #70a1dc;}";
    setStyleSheet(qss);
}

void SimulatorWidget::initSignalSlots()
{
    connect(start_button_, SIGNAL(toggled(bool)), this, SLOT(startSimulatorToggledSlot(bool)));
    connect(this, SIGNAL(laneIndexSignal(int, int)), road_branch_widget_, SLOT(laneIndexSlot(int, int)));
//    connect(this, SIGNAL(closeLightSignal()), road_branch_widget_, SLOT(closeLightSlot()));
    connect(this, SIGNAL(enableLaneIdCmbSignal(bool)), road_branch_widget_, SLOT(enableLaneIdCmbSlot(bool)));
    connect(send_msg_timer_, SIGNAL(timeout()), this, SLOT(sendMsgTimerTimeOutSlot()));
    connect(timer_, SIGNAL(timeout()), this, SLOT(timerTimeOutSlot()));
    connect(open_close_button_, SIGNAL(toggled(bool)), this, SLOT(openSerialTriggeredSlot(bool)));

    connect(detector_cfg_button_, SIGNAL(clicked()), this, SLOT(detectorEditButtonClicked()));
    connect(conn_button_, SIGNAL(clicked()), this, SLOT(connectButtonClicked()));
}

void SimulatorWidget::initComSettingLayout()
{
    QLabel *timespan_label = new QLabel(STRING_UI_TIMESPAN + "(s):");
    timespan_spinbox_ = new QSpinBox;
    timespan_spinbox_->setValue(1);
    start_button_ = new QPushButton(STRING_UI_START);
    start_button_->setCheckable(true);
    open_close_button_ = new QPushButton(STRING_UI_OPEN + STRING_UI_SERIALPORT);
    open_close_button_->setCheckable(true);
    open_tip_label_ = new QLabel;
    open_tip_label_->setMinimumWidth(20);
    txt_edit_ = new QTextEdit;

    port_cmb_ = new QComboBox;
    baud_rate_cmb_ = new QComboBox;
    data_bit_cmb_ = new QComboBox;
    stop_cmb_ = new QComboBox;
    parity_cmb_ = new QComboBox;

    port_cmb_->setMinimumWidth(80);
    baud_rate_cmb_->setMinimumWidth(80);
    data_bit_cmb_->setMinimumWidth(80);
    stop_cmb_->setMinimumWidth(80);
    parity_cmb_->setMinimumWidth(80);

    port_cmb_->addItem("com1");
    port_cmb_->addItem("com2");

    baud_rate_cmb_->addItem("4800");
    baud_rate_cmb_->addItem("9600");
    baud_rate_cmb_->addItem("14400");
    baud_rate_cmb_->addItem("19200");
    baud_rate_cmb_->addItem("38400");
    baud_rate_cmb_->setCurrentIndex(1);

    for (int i = 4; i < 9; i++)
    {
        data_bit_cmb_->addItem(QString::number(i));
    }
    int count = data_bit_cmb_->count();
    data_bit_cmb_->setCurrentIndex(count-1);

    stop_cmb_->addItem("1");
    stop_cmb_->addItem("1.5");
    stop_cmb_->addItem("2");
    stop_cmb_->setCurrentIndex(0);

    parity_cmb_->addItem("None");
    parity_cmb_->addItem("Odd");
    parity_cmb_->addItem("Even");
    parity_cmb_->addItem("Mark");
    parity_cmb_->addItem("Space");
    parity_cmb_->setCurrentIndex(0);

    QGroupBox *illustrate_grp = new QGroupBox;
    QString dir = MUtility::getImagesDir();
    QLabel *green_label = new QLabel;
    green_label->setPixmap(QPixmap(dir + "green_arrow.png"));
    QLabel *green_tip_label = new QLabel(STRING_UI_GREEN_TIP);
    QLabel *red_label = new QLabel;
    red_label->setPixmap(QPixmap(dir + "red_arrow.png"));
    QLabel *red_tip_label = new QLabel(STRING_UI_RED_TIP);

    QGridLayout *illustrate_glayout = new QGridLayout;
    illustrate_glayout->addWidget(green_label, 0, 0, 1, 1, Qt::AlignLeft);
    illustrate_glayout->addWidget(green_tip_label, 0, 1, 1, 1, Qt::AlignLeft);
    illustrate_glayout->addWidget(red_label, 1, 0, 1, 1, Qt::AlignLeft);
    illustrate_glayout->addWidget(red_tip_label, 1, 1, 1, 1, Qt::AlignLeft);
    illustrate_grp->setLayout(illustrate_glayout);

    QGridLayout *glayout = new QGridLayout;
    glayout->addWidget(new QLabel(STRING_UI_PORT + ":"), 0, 0, 1, 1, Qt::AlignCenter);
    glayout->addWidget(new QLabel(STRING_UI_BAUDRATE + ":"), 1, 0, 1, 1, Qt::AlignCenter);
    glayout->addWidget(new QLabel(STRING_UI_DATABIT + ":"), 2, 0, 1, 1, Qt::AlignCenter);
    glayout->addWidget(new QLabel(STRING_UI_STOPBIT + ":"), 3, 0, 1, 1, Qt::AlignCenter);
    glayout->addWidget(new QLabel(STRING_UI_PARITY + ":"), 4, 0, 1, 1, Qt::AlignCenter);

    glayout->addWidget(port_cmb_, 0, 1, 1, 1, Qt::AlignCenter);
    glayout->addWidget(baud_rate_cmb_, 1, 1, 1, 1, Qt::AlignCenter);
    glayout->addWidget(data_bit_cmb_, 2, 1, 1, 1, Qt::AlignCenter);
    glayout->addWidget(stop_cmb_, 3, 1, 1, 1, Qt::AlignCenter);
    glayout->addWidget(parity_cmb_, 4, 1, 1, 1, Qt::AlignCenter);
    QGroupBox *param_grp = new QGroupBox;
    param_grp->setLayout(glayout);

    QHBoxLayout *open_hlayout = new QHBoxLayout;
    open_hlayout->addWidget(open_close_button_, 0, Qt::AlignCenter);
    open_hlayout->addWidget(open_tip_label_, 0, Qt::AlignLeft);
    open_hlayout->addStretch(1);
    open_hlayout->addWidget(start_button_, 0, Qt::AlignCenter);

    QHBoxLayout *timespan_hlayout = new QHBoxLayout;
    timespan_hlayout->addWidget(timespan_label, 0, Qt::AlignLeft);
    timespan_hlayout->addWidget(timespan_spinbox_, 0, Qt::AlignLeft);

    QGroupBox *start_grp = new QGroupBox;
    QVBoxLayout *start_vlayout = new QVBoxLayout;
    start_vlayout->addWidget(txt_edit_);
    start_vlayout->addLayout(timespan_hlayout);
    start_vlayout->addLayout(open_hlayout);
    start_grp->setLayout(start_vlayout);

    com_setting_grp_ = new QGroupBox;
    QVBoxLayout *edit_vlayout = new QVBoxLayout(com_setting_grp_);
    edit_vlayout->addWidget(illustrate_grp);
    edit_vlayout->addStretch();
    edit_vlayout->addWidget(param_grp);
    edit_vlayout->addStretch();
    edit_vlayout->addWidget(start_grp);

    com_setting_grp_->setLayout(edit_vlayout);
}

void SimulatorWidget::initRoadbranchLayout()
{
    roadbranch_grp_ = new QGroupBox;
    QHBoxLayout *roadbranch_hlayout = new QHBoxLayout(roadbranch_grp_);
    road_branch_widget_ = new RoadBranchWidget(this);
    roadbranch_hlayout->addWidget(road_branch_widget_);
    roadbranch_grp_->setLayout(roadbranch_hlayout);
}

void SimulatorWidget::initScheduleInfoLayout()
{
    QGridLayout *glayout = new QGridLayout;
    glayout->addWidget(new QLabel(STRING_UI_SCHEDULE_ID+":"), 0, 0, 1, 1);
    glayout->addWidget(new QLabel(STRING_UI_EVENT_ID+":"), 1, 0, 1, 1);
    glayout->addWidget(new QLabel(STRING_UI_START_END_TIME+":"), 2, 0, 1, 1);
    glayout->addWidget(new QLabel(STRING_UI_CYCLE_TIME+":"), 3, 0, 1, 1);
    glayout->addWidget(new QLabel(STRING_UI_CTRL_MODE+":"), 4, 0, 1, 1);
    glayout->addWidget(new QLabel(STRING_UI_STAGE_ID+":"), 5, 0, 1, 1);
    glayout->addWidget(new QLabel(STRING_UI_CURRENT_PHASE+":"), 6, 0, 1, 1);

    sched_id_label_ = new QLabel;
    event_id_label_ = new QLabel;
    start_time_label_ = new QLabel;
    cycle_time_label_ = new QLabel;
    ctrl_mode_label_ = new QLabel;
    stage_id_label_ = new QLabel;
    curr_phase_id_label_ = new QLabel;

    glayout->addWidget(sched_id_label_, 0, 1, 1, 1);
    glayout->addWidget(event_id_label_, 1, 1, 1, 1);
    glayout->addWidget(start_time_label_, 2, 1, 1, 1);
    glayout->addWidget(cycle_time_label_, 3, 1, 1, 1);
    glayout->addWidget(ctrl_mode_label_, 4, 1, 1, 1);
    glayout->addWidget(stage_id_label_, 5, 1, 1, 1);
    glayout->addWidget(curr_phase_id_label_, 6, 1, 1, 1);

    signaler_time_label_ = new QLabel;
    count_down_lcd_ = new QLCDNumber;
    conn_button_ = new QPushButton(STRING_UI_CONNECT);
    detector_cfg_button_ = new QPushButton(STRING_UI_DETECTOR_EDIT);
    count_down_lcd_->setDigitCount(8);
    count_down_lcd_->setMinimumSize(QSize(0,32));
    count_down_lcd_->setFrameShape(QFrame::Box);
    count_down_lcd_->setFrameShadow(QFrame::Sunken);
    count_down_lcd_->setMouseTracking(Qt::ClickFocus);
    count_down_lcd_->setMode(QLCDNumber::Dec);
    count_down_lcd_->setSegmentStyle(QLCDNumber::Flat);
    count_down_lcd_->setToolTip(STRING_UI_GYR_LIGHT);
    count_down_lcd_->display("00-00-00");

    QVBoxLayout *signaler_vlayout = new QVBoxLayout;
    signaler_vlayout->addWidget(new QLabel(STRING_UI_PHASE_TIME + "(" + STRING_UI_GYR_LIGHT + "):"));
    signaler_vlayout->addWidget(count_down_lcd_);
    signaler_vlayout->addWidget(new QLabel(STRING_UI_SIGNALER_TIME + ":"));
    signaler_vlayout->addWidget(signaler_time_label_);
    signaler_vlayout->addWidget(detector_cfg_button_);
    signaler_vlayout->addWidget(conn_button_);

    QVBoxLayout *vlayout = new QVBoxLayout;
    vlayout->addLayout(glayout);
//    vlayout->addStretch();
    vlayout->addLayout(signaler_vlayout);

    schedule_grp_ = new QGroupBox;
    schedule_grp_->setLayout(vlayout);
}

bool SimulatorWidget::checkLaneId()
{
    QList<int> lane_id_list = road_branch_widget_->getLaneIdList();
    if (lane_id_list.contains(0))
    {
        QMessageBox::information(this, STRING_TIP, STRING_UI_EXISTS_INVALID_LANEID, STRING_OK);
        return false;
    }
    return true;
}

void SimulatorWidget::packComData(int lane_index)
{
    com_array_.clear();
    SerialData com_data;
    QList<int> lane_id_list = road_branch_widget_->getLaneIdList();
    curr_lane_id_ = lane_id_list.at(lane_index);
    need_leave_ = false;
    if (curr_lane_id_ >= 1 && curr_lane_id_ <= 48)
    {
        com_data.type = 0x01 + '\0';
        need_leave_ = true;
    }
    else if (curr_lane_id_ <= 56)
    {
        com_data.type = 0x04 + '\0';
    }
    else if (curr_lane_id_ <= 60)
    {
        com_data.type = 0x05 + '\0';
    }
    com_data.lane_id = curr_lane_id_ + '\0';
    int secs = QDateTime::currentDateTime().toTime_t();
    memcpy(com_data.ms_time, &secs, 4);

    com_array_.append(com_data.head);
    com_array_.append(com_data.type);
    com_array_.append(com_data.lane_id);
    com_array_.append(com_data.ms_time,2);
    com_array_.append(com_data.tail);

    qDebug() << "lane index:" << lane_index
             << "lane id:" << curr_lane_id_ << endl;
}

void SimulatorWidget::initMyComSetting()
{
    QString port_name = port_cmb_->currentText();
    QString baud_rate = baud_rate_cmb_->currentText();
    if (baud_rate == "4800")
    {
        my_com_setting_.BaudRate = BAUD4800;
    }
    else if (baud_rate == "9600")
    {
        my_com_setting_.BaudRate = BAUD9600;
    }
    else if (baud_rate == "19200")
    {
        my_com_setting_.BaudRate = BAUD19200;
    }
    else if (baud_rate == "38400")
    {
        my_com_setting_.BaudRate = BAUD38400;
    }

    int data_bit = data_bit_cmb_->currentText().toInt();
    if (data_bit == 5)
    {
        my_com_setting_.DataBits = DATA_5;
    }
    else if (data_bit == 6)
    {
        my_com_setting_.DataBits = DATA_6;
    }
    else if (data_bit == 7)
    {
        my_com_setting_.DataBits = DATA_7;
    }
    else if (data_bit == 8)
    {
        my_com_setting_.DataBits = DATA_8;
    }

    QString stop_bit = stop_cmb_->currentText();
    if (stop_bit == "1")
    {
        my_com_setting_.StopBits = STOP_1;
    }
    else if (stop_bit == "1.5")
    {
        my_com_setting_.StopBits = STOP_1_5;
    }
    else if (stop_bit == "2")
    {
        my_com_setting_.StopBits = STOP_2;
    }

    QString parity_str = parity_cmb_->currentText();
    if (parity_str == "None")
    {
        my_com_setting_.Parity = PAR_NONE;
    }
    else if (parity_str == "Odd")
    {
        my_com_setting_.Parity = PAR_ODD;
    }
    else if (parity_str == "Event")
    {
        my_com_setting_.Parity = PAR_EVEN;
    }
    else if (parity_str == "Mark")
    {
        my_com_setting_.Parity = PAR_MARK;
    }
    else if (parity_str == "Space")
    {
        my_com_setting_.Parity = PAR_SPACE;
    }

    my_com_->setPortName(port_name);
    my_com_->setBaudRate(my_com_setting_.BaudRate);
    my_com_->setDataBits(my_com_setting_.DataBits);
    my_com_->setStopBits(my_com_setting_.StopBits);
    my_com_->setParity(my_com_setting_.Parity);
}

QString SimulatorWidget::formatComData(const QByteArray &array)
{
    QString str;
    QByteArray tmp = array.toHex();
    for (int i = 0; i < 6; i++)
    {
        str.append(tmp.left(2));
        str.append(" ");
        tmp.remove(0,2);
    }
    str = str.toUpper();
    return str;
}

void SimulatorWidget::enableComSetting(bool enable)
{
    port_cmb_->setEnabled(enable);
    baud_rate_cmb_->setEnabled(enable);
    data_bit_cmb_->setEnabled(enable);
    stop_cmb_->setEnabled(enable);
    parity_cmb_->setEnabled(enable);
}

void SimulatorWidget::dumpComData()
{
    SerialData com_data;
//    com_data.head = com_array_.at(0);
    com_data.type = com_array_.at(1);
    com_data.lane_id = com_array_.at(2);
    com_data.ms_time[0] = com_array_.at(3);
    com_data.ms_time[1] = com_array_.at(4);
//    com_data.tail = com_array_.at(5);

    int ts = 0;
    memcpy(&ts, com_data.ms_time,4);
    QString head = QString("0x%1").arg(int(com_data.head),0,16);
    QString tail = QString("0x%1").arg(int(com_data.tail),0,16);
    qDebug() << "head:" << head
             << "type:" << com_data.type - '\0'
             << "lane_id:" << com_data.lane_id - '\0'
             << "timespan:" << ts
             << "tail:" << tail
             << endl;
}

void SimulatorWidget::test()
{
//    com_array_.clear();
//    com_array_.append("F10101ed");
    QFile file("test.dat");
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    file.write(com_array_);
    file.close();
}
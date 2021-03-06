#include "simulatorwidget.h"
#include "macrostrings.h"
#include "serialdata.h"
#include "mutility.h"
#include "detectorideditwidget.h"
#include "xmlhelper.h"
#include "synccommand.h"
#include "filereaderwriter.h"

#include "testdlg.h"

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

#define CONN_WAIT_MS    3000
#define VERSION_CHECK_MS    5000

#define SIGNALER_TIME_UPDATE(str) \
    signaler_time_label_->setText("<font size=4>" + str + "</font>");

SimulatorWidget::SimulatorWidget(QWidget *parent) :
    QWidget(parent)
{
    struct PortSettings my_com_setting_ = {BAUD9600, DATA_8, PAR_NONE, STOP_1, FLOW_OFF, 500};
    my_com_ = new Win_QextSerialPort("com1", my_com_setting_, QextSerialBase::EventDriven);
    detector_edit_dlg_ = new DetectorIdEditWidget(this);

    curr_lane_idx_ = 0;
    curr_lane_id_ = 0;
    serial_status_ = false;
    send_msg_timer_ = new QTimer(this);
    timer_ = new QTimer(this);
    need_leave_ = false;

    port_ = 0;
    sync_cmd_ = SyncCommand::GetInstance();
    conn_status_ = false;
    ver_check_id_ = 0;
    is_inited_ = false;
    ui_timer_id_ = 0;
    is_uitimer_started_ = false;

    curr_stage_id_ = 0;
    total_stage_count_ = 0;
    count_down_secs_ = 0;
    count_down_light_ = 4;

    conn_timer_ = new QTimer(this);
    count_down_timer_ = new QTimer(this);
    signaler_timer_ = new QTimer(this);
    sec_count_ = 0;

    test_dlg_ = new TestDlg(this);
    pre_lane_idx_ = 0;

    db_ptr_ = MDatabase::GetInstance();
    phase_handler_ = new PhaseHandler;

    is_first_ = true;
    is_first_send_ = true;
    is_first_end_ = true;

    setWindowTitle(STRING_UI_WINDOW_TITLE);
    initPage();
    initSignalSlots();
    initCtrlModeDesc();
    initPreDetectorColorList();
    initRedDetectorFlagList();
    initTrafficDispatcher();
    setFixedSize(826+170,606);
    start_button_->setEnabled(false);
}

SimulatorWidget::~SimulatorWidget()
{
    if (timer_ != NULL)
    {
        delete timer_;
        timer_ = NULL;
    }
    if (db_ptr_ != NULL)
    {
        db_ptr_->DestroyInstance();
    }
    if (phase_handler_ != NULL)
    {
        delete phase_handler_;
        phase_handler_ = NULL;
    }
}

void SimulatorWidget::initialize()
{
    XmlHelper *helper = XmlHelper::GetInstance();
    if (helper == NULL)
    {
        return;
    }
    QString dir = MUtility::getConfigDir();
    bool state = helper->LoadXmlFile(dir + "app.config");
    if (!state)
    {
        QMessageBox::information(this, STRING_TIP, STRING_UI_OPEN + STRING_FAILED, STRING_OK);
        return;
    }
    ip_ = helper->ParseXmlNodeContent("ip");
    port_ = helper->ParseXmlNodeContent("port").toInt();
    ip_lineedit_->setText(ip_);
    port_lineedit_->setText(QString::number(port_));
    QString str = date_time_.toString("yyyy-MM-dd hh:mm:ss");
    SIGNALER_TIME_UPDATE(str)

    cfg_file_ = MUtility::getTempDir() + ip_ + ".dat";

    show();
}

void SimulatorWidget::startSimulatorToggledSlot(bool checked)
{
    if (!checkLaneId())
    {
        start_button_->setChecked(!checked);
        return;
    }
    if (checked && !conn_status_)
    {
        start_button_->setChecked(!checked);
        QMessageBox::information(this, STRING_TIP, STRING_UI_START_WITHOUT_CONN, STRING_OK);
        return;
    }
    int phase_id = curr_phase_id_label_->text().trimmed().toInt();
    if (checked && phase_id == 0)
    {
        start_button_->setChecked(!checked);
        QMessageBox::information(this, STRING_TIP, STRING_UI_PHASE_ID_INVALID, STRING_OK);
        return;
    }
    if (checked)
    {
        initMyComSetting();
        emit enableDetectorIdCmbSignal(false);
        timespan_spinbox_->setEnabled(false);
        start_button_->setText(STRING_UI_STOP);
        if (!simualtorComdataDispatcher())
        {
            return;
        }
        int secs = timespan_spinbox_->value();
        send_msg_timer_->start(secs*1000);
//        if (need_leave_)
//        {
//            timer_->start(qrand() % 1000 + 1000);
//        }
//        txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
    }
    else
    {
        start_button_->setText(STRING_UI_START);
        send_msg_timer_->stop();
        timer_->stop();
        enableComSetting(true);
        emit enableDetectorIdCmbSignal(true);
        timespan_spinbox_->setEnabled(true);
    }
}

void SimulatorWidget::sendMsgTimerTimeOutSlot()
{
    if (timer_->isActive())
    {
        return;
    }
    qDebug() << "new car come";
    startSimulatorToggledSlot(true);
}
/* 1. 检查当前车道是否在当前相位的所控制的通道中且为可通行状态
 * 2. 如果可通行则放行，否则等待计时器清零，什么也不做。
*/
void SimulatorWidget::timerTimeOutSlot()
{
    timer_->stop();
    unsigned int phase_id = curr_phase_id_label_->text().trimmed().toUInt();
    for (int i = 0; i < 12; i++)
    {
        int pre_idx =  pre_lane_idx_list_.at(i);
        if (isChannelAccessible(phase_id, i+1) && pre_idx >= 0)
        {
            qDebug() << "Leave--(phase_id:" << phase_id << ", channel_id:" << i+1 << ") accessible" ;
            if ((car_sent_list_.at(i) == Go) && need_leave_list_.at(i))
            {
                qDebug() << "Leave--close pre detector:" << pre_idx+1 << ",Color:" << colorPrintable(channel_detector_color_list_.at(pre_idx))
                         << "false, light detector:" << pre_idx+1 << "Color: Red";
                packComData(pre_lane_idx_list_.at(i));
                need_leave_list_[pre_lane_idx_list_.at(i)] = false;
                com_array_[1] = 0x02 + '\0';
                char ms[4] = {'\0'};
                int secs = QDateTime::currentDateTime().toTime_t();
                memcpy(ms, &secs, sizeof(secs));
                com_array_[3] = ms[0];
                com_array_[4] = ms[1];
                txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
                emit showLaneDetectorSignal(pre_idx, channel_detector_color_list_.at(pre_idx), false);
                emit showLaneDetectorSignal(pre_idx, RoadBranchWidget::Red, true);
                channel_detector_color_list_[i] = Red;
                car_sent_list_[i] = None;
                my_com_->write(com_array_);
            }
            else if ((car_sent_list_.at(i) == None) && !need_leave_list_.at(i))
            {
                qDebug() << "Leave--close only pre detector:" << pre_lane_idx_list_.at(i)+1 << ",Color:" << colorPrintable(channel_detector_color_list_.at(i)) << " false";
                packComData(pre_lane_idx_list_.at(i));
                need_leave_list_[pre_lane_idx_list_.at(i)] = false;
                txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
                emit showLaneDetectorSignal(pre_lane_idx_list_.at(i), channel_detector_color_list_.at(i), false);
                channel_detector_color_list_[i] = Off;
                car_sent_list_[i] = Other;
                my_com_->write(com_array_);
            }
        }
    }

    /*
    timer_->stop();
    unsigned int phase_id = curr_phase_id_label_->text().trimmed().toInt();
    QList<unsigned char> channel_id_list = phase_handler_->get_phase_ctrled_channel_list(phase_id);
    if (channel_id_list.contains(pre_lane_idx_+1))
    {
        // TODO: send msg
        packComData(pre_lane_idx_);
        need_leave_ = false;
        com_array_[1] = 0x02 + '\0';
        char ms[4] = {'\0'};
        int secs = QDateTime::currentDateTime().toTime_t();
        memcpy(ms, &secs, sizeof(secs));
        com_array_[3] = ms[0];
        com_array_[4] = ms[1];
        txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
        emit showLaneDetectorSignal(curr_lane_idx_, RoadBranchWidget::Green, false);
        emit showLaneDetectorSignal(curr_lane_idx_, RoadBranchWidget::Red, true);
        detector_red_flag_list_[curr_lane_idx_] = 0;
        my_com_->write(com_array_);
    }
    else
    {
        return;
    }*/
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
//    detector_edit_dlg_->exec();
//    road_branch_widget_->showDetectorSlot(qrand()%17, RoadBranchWidget::LightColor(qrand()%4),true);
}

void SimulatorWidget::connectButtonClicked()
{
#if 0
    test_dlg_->setPtr(road_branch_widget_);
    test_dlg_->exec();
#endif
#if 1
    ip_ = ip_lineedit_->text();
    ip_ = MUtility::trimmedAll(ip_);
    port_ = port_lineedit_->text().toInt();
    conn_tip_label_->clear();
    if (conn_status_)
    {
        if (ver_check_id_ == 0 && is_first_end_)
        {
            sync_cmd_->StopMonitoring();
            is_first_end_ = false;
        }
        sync_cmd_->disconnectFromHost();
        if (ver_check_id_ != 0)
        {
            killTimer(ver_check_id_);
            ver_check_id_ = 0;
            conn_status_ = false;
        }
        conn_tip_label_->setText(STRING_NETWORK_DISCONNECTED);
        conn_button_->setText(STRING_UI_CONNECT);
        start_button_->setChecked(false);
    }
    else
    {
        sync_cmd_->connectToHost(ip_, port_);
        conn_timer_->start(CONN_WAIT_MS);
        conn_button_->setEnabled(false);
        conn_tip_label_->setText(STRING_UI_CONNECT + "...");
    }

    clear_status_button_->setEnabled(conn_button_->text() != STRING_UI_CONNECT);

#endif
}

void SimulatorWidget::clearButtonClicked()
{
    // TODO: clear traffic light status
    road_branch_widget_->closeLightSlot();
}

void SimulatorWidget::connectEstablishedSlot()
{
    conn_status_ = true;
    conn_button_->setEnabled(true);
    conn_button_->setText(STRING_UI_DISCONNECT);
    conn_timer_->stop();
    sync_cmd_->ReadTscVersion(this, SLOT(onCmdGetVerIdSlot(QByteArray&)));
    conn_tip_label_->setText(STRING_NETWORK_VERSION_CHECK);
    if (ver_check_id_ == 0)
    {
        ver_check_id_ = startTimer(VERSION_CHECK_MS);
    }
}

void SimulatorWidget::disconnectedSlot()
{
    if (ver_check_id_ != 0)
    {
        killTimer(ver_check_id_);
        ver_check_id_ = 0;
    }
    conn_status_ = false;
    conn_tip_label_->setText(STRING_NETWORK_DISCONNECTED);
    conn_button_->setText(STRING_UI_CONNECT);
    clear_status_button_->setEnabled(true);
}

void SimulatorWidget::connectErrorSlot(const QString &str)
{
    conn_tip_label_->setText(str);
}

void SimulatorWidget::onCmdGetVerIdSlot(QByteArray &array)
{
    char ver[12] = {'\0'};
    memcpy(ver, array.data(), 11);
    if (strcmp(ver, "CYT0V100END") != 0)
    {
        conn_tip_label_->setText(STRING_NETWORK_VERSION_ERROR);
        return;
    }

    if (ver_check_id_ != 0)
    {
        killTimer(ver_check_id_);
        ver_check_id_ = 0;
    }
    conn_tip_label_->setText(STRING_NETWORK_VERSION_RIGHT);
//    sync_cmb_->ReleaseSignalSlots();
    cfg_array_.clear();
    recv_array_.clear();
    count_down_timer_->start(1000);
    sync_cmd_->ReadSignalerConfigFile(this, SLOT(onCmdReadConfigFileSlot(QByteArray&)));
}

void SimulatorWidget::onCmdReadConfigFileSlot(QByteArray &array)
{
    cfg_array_.append(array);
    if (!cfg_array_.left(4).contains("CYT4"))
    {
        cfg_array_.clear();
        sync_cmd_->ReadEventLogFile(this, SLOT(onCmdReadConfigFileSlot(QByteArray&)));
        return;
    }
    if (!cfg_array_.contains("END"))
    {
        return;
    }
    bool res = parseConfigContent(cfg_array_);
    int ret = -1;
    if (!res)
    {
        ret = QMessageBox::question(this, STRING_TIP, STRING_NETWORK_CONFIG_RETRY, STRING_YES, STRING_NO);
        if (ret == 0)
        {
            cfg_array_.clear();
            sync_cmd_->ReadSignalerConfigFile(this, SLOT(onCmdReadConfigFileSlot(QByteArray&)));
        }
        else
        {
            cfg_array_.clear();
            // TODO: update ui
            if (initTscParam())
            {
                updateScheduleInfo();
            }
            return;
        }
    }
    else
    {
        QFile file(cfg_file_);
retry:  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            ret = QMessageBox::question(this, STRING_TIP, STRING_NETWORK_CFG_TEMP_SAVE, STRING_YES, STRING_NO);
            if (ret == 0)
            {
                goto retry;
            }
            else
            {
                QFile::remove(cfg_file_);
            }
        }
        else
        {
            file.write(cfg_array_);
            file.close();
            // TODO: update schedule info
            if (initTscParam())
            {
                updateScheduleInfo();
            }
            sync_cmd_->GetLightStatus(this, SLOT(onCmdParseParam(QByteArray&)));
        }
    }
}

void SimulatorWidget::onCmdParseParam(QByteArray &array)
{
    recv_array_.append(array);
    if (!checkPackage(recv_array_))
    {
        return;
    }
    if (!(recv_array_.left(3).contains("CYT") && recv_array_.contains("END")))
    {
        return;
    }
    bool status = false;
    char cmd_id = '\0';
    while (true)
    {
        if (recv_array_.size() < 4)
        {
            break;
        }
        if (!(recv_array_.contains("CYT") || recv_array_.contains("END")))
        {
            recv_array_.clear();
            break;
        }
        cmd_id = recv_array_.at(3);
        switch (cmd_id)
        {
        case '0':
            break;
        case '1':
            status = parseBeginMonitorContent(recv_array_);
            if (!status)
            {
                QMessageBox::information(this, STRING_TIP, STRING_NETWORK_PARSE_ERROR + "(CYT1)", STRING_OK);
            }
            break;
        case '2':
            break;
        case '3':
            status = parseLightStatusContent(recv_array_);
            if (!status)
            {
                QMessageBox::information(this, STRING_TIP, STRING_NETWORK_PARSE_ERROR + "(CYT3)", STRING_OK);
            }
            else if (is_first_)
            {
                sync_cmd_->GetTscTime();
                if (!is_uitimer_started_)
                {
                    ui_timer_id_ = startTimer(3600*1000);
                    is_uitimer_started_ = true;
                }
                is_first_ = false;
            }
            break;
        case '4':
            status = parseConfigContent(recv_array_);
            if (status)
            {
                QFile file(cfg_file_+".tmp");
                file.open(QIODevice::WriteOnly);
                file.write(recv_array_);
                file.close();
                recv_array_.clear();
            }
            break;
        case '5':
            status = parseCountDownContent(recv_array_);
            if (!status)
            {
                QMessageBox::information(this, STRING_OK, STRING_NETWORK_PARSE_ERROR + "(CYT5)", STRING_OK);
            }
            break;
        case '6':
            break;
        case '7':
            status = parseTSCTimeContent(recv_array_);
            if (!status)
            {
                QMessageBox::information(this, STRING_TIP, "Parse tsc time failed.", STRING_OK);
            }
            else/* if (!is_inited_)*/
            {
                if (initTscParam())
                {
                    updateScheduleInfo();
                }
                signaler_timer_->start(1000);
                sync_cmd_->StartMonitoring();
                is_inited_ = true;
            }
            break;
        case '8':
            break;
        case '9':
            status = parseDetectorFlowContent(recv_array_);
            break;
        case 'A':
            status = parseDetectorFaultContent(recv_array_);
            break;
        case 'B':
            status = parseRealTimeFlowContent(recv_array_);
            break;
        case 'C':
            status = parseDriverStatusContent(recv_array_);
            break;
        case 'D':
            status = parseDriverRealtimeStatusContent(recv_array_);
            break;
        case 'E':
            status = parseLightRealTimeStatusContent(recv_array_);
            break;
        case 'F':
            status = parseAllLightOnContent(recv_array_);
            if (!status)
            {
                QMessageBox::information(this, STRING_TIP, STRING_NETWORK_PARSE_ERROR + "(CYTF)", STRING_OK);
            }
            break;
        default:
            break;
        }
    }
}

void SimulatorWidget::connTimerTimeoutSlot()
{
    conn_timer_->stop();
    if (sync_cmd_->isConnectionValid())
    {
        sync_cmd_->closeConnection();
        conn_tip_label_->setText(STRING_NETWORK_HOST_UNREACHABLE);
        conn_button_->setEnabled(true);
    }
}

void SimulatorWidget::signalerTimerTimeoutSlot()
{
    date_time_ = date_time_.addSecs(1);
    QString str = date_time_.toString("yyyy-MM-dd hh:mm:ss");
    SIGNALER_TIME_UPDATE(str)
    sec_count_++;
    if (sec_count_ % 3 == 0)
    {
        updateScheduleInfo();
        sec_count_ = 0;
    }
}

void SimulatorWidget::countDownTimerTimeoutSlot()
{
#define FORMAT_COUNT_DOWN(text, count_down) \
    if (count_down != 0) { --count_down; } \
    str.sprintf(text, count_down);

    QString str;
    switch (count_down_light_)
    {
    case Red:
        FORMAT_COUNT_DOWN("00-00-%02d", count_down_secs_)
        break;
    case Yellow:
        FORMAT_COUNT_DOWN("00-%02d-00", count_down_secs_)
        break;
    case Green:
        FORMAT_COUNT_DOWN("%02d-00-00", count_down_secs_)
        break;
    case Off:
    default:
        str = "00-00-00";
        break;
    }
    count_down_lcd_->display(str);
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
    if (!(ver_check_id_ == 0 && conn_status_ == false))
    {
        sync_cmd_->StopMonitoring();
    }
    else
    {
        killTimer(ver_check_id_);
        ver_check_id_ = 0;
    }
    sync_cmd_->disconnectFromHost();
    XmlHelper *helper = XmlHelper::GetInstance();
    ip_ = MUtility::trimmedAll(ip_lineedit_->text());
    QString str = port_lineedit_->text().trimmed();
    helper->SetXmlNodeContent("ip", ip_);
    helper->SetXmlNodeContent("port", str);
    helper->DestroyInstance();
}

void SimulatorWidget::timerEvent(QTimerEvent *)
{
    if (ver_check_id_ != 0)
    {
        connectEstablishedSlot();
    }
    if (ui_timer_id_ != 0)
    {
        sync_cmd_->GetTscTime();
    }
}

void SimulatorWidget::initPage()
{
    initComSettingLayout();
    initRoadbranchLayout();
    initScheduleInfoLayout();

    clear_status_button_->setEnabled(conn_button_->text() != STRING_UI_CONNECT);

    QHBoxLayout *hlayout = new QHBoxLayout;
    hlayout->addWidget(com_setting_grp_);
    hlayout->addWidget(roadbranch_grp_);
    hlayout->addWidget(schedule_grp_);
    hlayout->setStretch(0,1);
    hlayout->setStretch(1,4);
    hlayout->setStretch(2,1);
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
    connect(this, SIGNAL(showLightSignal(int, int)), road_branch_widget_, SLOT(laneIndexSlot(int, int)));
//    connect(this, SIGNAL(closeLightSignal()), road_branch_widget_, SLOT(closeLightSlot()));
    connect(this, SIGNAL(enableDetectorIdCmbSignal(bool)), road_branch_widget_, SLOT(enableDetectorIdCmbSlot(bool)));
    connect(send_msg_timer_, SIGNAL(timeout()), this, SLOT(sendMsgTimerTimeOutSlot()));
    connect(timer_, SIGNAL(timeout()), this, SLOT(timerTimeOutSlot()));
    connect(open_close_button_, SIGNAL(toggled(bool)), this, SLOT(openSerialTriggeredSlot(bool)));

    connect(detector_cfg_button_, SIGNAL(clicked()), this, SLOT(detectorEditButtonClicked()));
    connect(conn_button_, SIGNAL(clicked()), this, SLOT(connectButtonClicked()));
    connect(clear_status_button_, SIGNAL(clicked()), this, SLOT(clearButtonClicked()));

    connect(sync_cmd_, SIGNAL(connectedSignal()), this, SLOT(connectEstablishedSlot()));
//    connect(sync_cmd_, SIGNAL(connectErrorStrSignal(QString)), this, SLOT(connectErrorSlot(QString)));
    connect(sync_cmd_, SIGNAL(disconnectedSignal()), this, SLOT(disconnectedSlot()));
    connect(sync_cmd_, SIGNAL(connectErrorStrSignal(QString)), conn_tip_label_, SLOT(setText(QString)));

    connect(conn_timer_, SIGNAL(timeout()), this, SLOT(connTimerTimeoutSlot()));
    connect(signaler_timer_, SIGNAL(timeout()), this, SLOT(signalerTimerTimeoutSlot()));
    connect(count_down_timer_, SIGNAL(timeout()), this, SLOT(countDownTimerTimeoutSlot()));

    connect(this, SIGNAL(showLaneDetectorSignal(int,int,bool)), road_branch_widget_, SLOT(showDetectorSlot(int,int,bool)));

    // just for unit testing use
    connect(test_dlg_, SIGNAL(showChannelLightSignal(int,int)), road_branch_widget_, SLOT(laneIndexSlot(int,int)));
    connect(test_dlg_, SIGNAL(showLaneDetectorSignal(int,int,bool)), road_branch_widget_, SLOT(showDetectorSlot(int,int,bool)));
    connect(test_dlg_, SIGNAL(showSidewalkDetectorSignal(int,int,bool)), road_branch_widget_, SLOT(showDetectorSlot(int,int,bool)));
    connect(test_dlg_, SIGNAL(sendNetworkByteArray(QByteArray&)), this, SLOT(onCmdParseParam(QByteArray&)));
}

void SimulatorWidget::initComSettingLayout()
{
    QLabel *timespan_label = new QLabel(STRING_UI_TIMESPAN + "(s):");
    timespan_spinbox_ = new QSpinBox;
    timespan_spinbox_->setRange(3, 65535);
    timespan_spinbox_->setValue(2);
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
//    baud_rate_cmb_->addItem("14400");
    baud_rate_cmb_->addItem("19200");
//    baud_rate_cmb_->addItem("38400");
    baud_rate_cmb_->addItem("115200");
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
    green_label->setPixmap(QPixmap(dir + "detector_green_img.png"));
    QLabel *green_tip_label = new QLabel(STRING_UI_GREEN_TIP);
    QLabel *red_label = new QLabel;
    red_label->setPixmap(QPixmap(dir + "detector_red_img.png"));
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
    com_setting_grp_->setMaximumWidth(190);
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

    sched_id_label_ = new QLabel(" -");
    event_id_label_ = new QLabel(" -");
    start_time_label_ = new QLabel("--:--");
    cycle_time_label_ = new QLabel(" -");
    ctrl_mode_label_ = new QLabel(" -");
    stage_id_label_ = new QLabel(" -");
    curr_phase_id_label_ = new QLabel(" -");

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
    clear_status_button_ = new QPushButton(STRING_UI_CLEAR_STATUS);
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
//    signaler_vlayout->addWidget(detector_cfg_button_);
//    signaler_vlayout->addWidget(conn_button_);

    QVBoxLayout *vlayout = new QVBoxLayout;
    vlayout->addLayout(glayout);
//    vlayout->addStretch();
    vlayout->addLayout(signaler_vlayout);

    QGroupBox *sched_grp = new QGroupBox;
    sched_grp->setLayout(vlayout);

    QGridLayout *network_glayout = new QGridLayout;
    ip_lineedit_ = new QLineEdit;
    ip_lineedit_->setInputMask("000.000.000.000");
    ip_lineedit_->setMinimumWidth(95);
    port_lineedit_ = new QLineEdit;
    port_lineedit_->setMinimumWidth(95);
    port_lineedit_->setValidator(new QIntValidator(0, 65535, port_lineedit_));
    conn_tip_label_ = new QLabel;
    network_glayout->addWidget(new QLabel(STRING_UI_IP + ":"), 0, 0, 1, 1);
    network_glayout->addWidget(new QLabel(STRING_UI_PORT + ":"), 1, 0, 1, 1);
    network_glayout->addWidget(ip_lineedit_, 0, 1, 1, 3);
    network_glayout->addWidget(port_lineedit_, 1, 1, 1, 3);
    network_glayout->addWidget(conn_button_, 2, 0, 1, 1);
    network_glayout->addWidget(clear_status_button_, 2, 3, 1, 1);
    network_glayout->addWidget(conn_tip_label_, 3, 0, 1, 1);

    QGroupBox *network_grp = new QGroupBox;
    network_grp->setLayout(network_glayout);

    QVBoxLayout *all_vlayout = new QVBoxLayout;
    all_vlayout->addWidget(sched_grp);
    all_vlayout->addWidget(network_grp);
    all_vlayout->setStretch(0, 4);
    all_vlayout->setStretch(1, 1);
    schedule_grp_ = new QGroupBox;
    schedule_grp_->setLayout(all_vlayout);
    schedule_grp_->setMaximumWidth(200);
}

void SimulatorWidget::initCtrlModeDesc()
{
    ctrl_mode_desc_map_.insert(0, STRING_CTRL_GIVEN_CYCLE);
    ctrl_mode_desc_map_.insert(1, STRING_CTRL_CLOSE_LIGHT);
    ctrl_mode_desc_map_.insert(2, STRING_CTRL_YELLOW_FLASH);
    ctrl_mode_desc_map_.insert(3, STRING_CTRL_ALL_RED);
    ctrl_mode_desc_map_.insert(4, STRING_CTRL_COORDINATE);
    ctrl_mode_desc_map_.insert(5, STRING_CTRL_FULL_INDUCTION);
    ctrl_mode_desc_map_.insert(6, STRING_CTRL_MAIN_HALF_INDUCTION);
    ctrl_mode_desc_map_.insert(7, STRING_CTRL_SECOND_HALF_INDUC);
    ctrl_mode_desc_map_.insert(8, STRING_CTRL_SINGLE_ADAPT);
    ctrl_mode_desc_map_.insert(9, STRING_CTRL_CROSS_STREET);
    ctrl_mode_desc_map_.insert(10, STRING_CTRL_COOR_INDUCTION);
    ctrl_mode_desc_map_.insert(27, STRING_CTRL_BUS_FIRST);
    ctrl_mode_desc_map_.insert(28, STRING_CTRL_TRAFFIC_CTRL);
    ctrl_mode_desc_map_.insert(29, STRING_CTRL_MANUAL_CTRL);
    ctrl_mode_desc_map_.insert(30, STRING_CTRL_SYS_FAULT_YELLOW);

    for (int i = 1; i < 17; i++)
    {
        phase_id_list_.append(i);
    }
}

bool SimulatorWidget::initTscParam()
{
    FileReaderWriter reader;
    bool res = reader.ReadFile(cfg_file_.toStdString().c_str(), tsc_param_);
    if (!res)
    {
        QMessageBox::information(this, STRING_TIP, STRING_UI_OPEN_CONFIG + STRING_FAILED, STRING_OK);
        return false;
    }
    reader.ReadFile(db_ptr_, cfg_file_.toStdString().c_str());
    phase_handler_->init_database((void*)db_ptr_);
    phase_handler_->init();
    return true;
}

void SimulatorWidget::updateScheduleInfo()
{
    unsigned char sched_id = 0;
    unsigned char time_section_id = 0;
    QDate curr_date = date_time_.date();
    QTime curr_time = date_time_.time();
    int m = 0;
    for (m = 0; m < tsc_param_.sched_table_.FactScheduleNum; m++)
    {
        if ((tsc_param_.sched_table_.ScheduleList[m].ScheduleMonth & (0x01 << curr_date.month())) == 0)
        {
            continue;
        }
        if (curr_date.dayOfWeek() == 7)
        {
            if ((tsc_param_.sched_table_.ScheduleList[m].ScheduleWeek & (0x01 << 1)) == 0)
            {
                continue;
            }
        }
        else
        {
            if ((tsc_param_.sched_table_.ScheduleList[m].ScheduleDay & (0x01 << curr_date.day())) == 0)
            {
                continue;
            }
        }
        if ((tsc_param_.sched_table_.ScheduleList[m].ScheduleDay & (0x01 << curr_date.day())) == 0)
        {
            continue;
        }
        sched_id = tsc_param_.sched_table_.ScheduleList[m].ScheduleId;
        time_section_id = tsc_param_.sched_table_.ScheduleList[m].TimeSectionId;
        sched_id_label_->setText(QString::number(sched_id));
        break;
    }   //:~ schedule id

    QString str;
    int n = 0;
    unsigned char pattern_id = 0;
//    ctrl_mode_label_->setText(" -");
    for (m = 0; m < tsc_param_.time_section_table_.FactTimeSectionNum; m++)
    {
        if (tsc_param_.time_section_table_.TimeSectionList[m][0].TimeSectionId != time_section_id)
        {
            continue;
        }
        for (n = 0; n < tsc_param_.time_section_table_.FactEventNum; n++)
        {
            unsigned char start_hour = tsc_param_.time_section_table_.TimeSectionList[m][n].StartHour;
            unsigned char start_min = tsc_param_.time_section_table_.TimeSectionList[m][n].StartMinute;
            if (start_hour > curr_time.hour() || (start_hour == curr_time.hour() && start_min > curr_time.minute()))
            {
                if (n == 0)
                {
                    event_id_label_->setText(" -");
                    start_time_label_->setText("--:--");
                    ctrl_mode_label_->setText(" -");
                    pattern_id = 0;
                }
                else
                {
                    unsigned char event_id = tsc_param_.time_section_table_.TimeSectionList[m][n-1].EventId;
                    start_hour = tsc_param_.time_section_table_.TimeSectionList[m][n-1].StartHour;
                    start_min = tsc_param_.time_section_table_.TimeSectionList[m][n-1].StartMinute;
//                    unsigned char ctrl_mode = tsc_param_.time_section_table_.TimeSectionList[m][n-1].ControlMode;
                    QString str;
                    event_id_label_->setText(str.sprintf("%d", event_id));
                    str.sprintf("%02d:%02d", start_hour, start_min);
                    start_time_label_->setText(str);
//                    str = ctrl_mode_desc_map_.value(ctrl_mode); //EventLogDescriptor::GetInstance()->get_ctrl_mode_desc(ctrl_mode);
//                    ctrl_mode_label_->setText(str);
                    pattern_id = tsc_param_.time_section_table_.TimeSectionList[m][n-1].PatternId;
                }
                break;
            }
            if (tsc_param_.time_section_table_.TimeSectionList[m][n+1].EventId == 0)
            {
                unsigned char event_id = tsc_param_.time_section_table_.TimeSectionList[m][n].EventId;
                str.sprintf("%d", event_id);
                event_id_label_->setText(str);
                start_hour = tsc_param_.time_section_table_.TimeSectionList[m][n].StartHour;
                start_min = tsc_param_.time_section_table_.TimeSectionList[m][n].StartMinute;
                str.sprintf("%02d:%02d", start_hour, start_min);
                start_time_label_->setText(str);
                unsigned char ctrl_mode = tsc_param_.time_section_table_.TimeSectionList[m][n].ControlMode;
                str = ctrl_mode_desc_map_.value(ctrl_mode);//EventLogDescriptor::GetInstance()->get_ctrl_mode_desc(ctrl_mode);
                pattern_id = tsc_param_.time_section_table_.TimeSectionList[m][n].PatternId;
                break;
            }
        }
        break;
    }   //:~ event id, start time, control mode desc

    unsigned char time_config_id = 0;
    for (m = 0; m < tsc_param_.timing_plan_table_.FactPatternNum; m++)
    {
        if (tsc_param_.timing_plan_table_.PatternList[m].PatternId == pattern_id)
        {
            unsigned short circle_time = tsc_param_.timing_plan_table_.PatternList[m].CycleTime;
            str.sprintf("%d", circle_time);
            cycle_time_label_->setText(str);
            time_config_id = tsc_param_.timing_plan_table_.PatternList[m].TimeConfigId;
        }
    }
    unsigned char stage_count = 0;
    for (m = 0; m < tsc_param_.stage_timing_table_.FactTimeConfigNum; m++)
    {
        if (tsc_param_.stage_timing_table_.TimeConfigList[m][0].TimeConfigId != time_config_id)
        {
            continue;
        }
        for (n = 0; n < tsc_param_.stage_timing_table_.FactStageNum; n++)
        {
//            stage_phase_buff[n] = tsc_param_.stage_timing_table_.TimeConfigList[m][n].PhaseId;
            if (tsc_param_.stage_timing_table_.TimeConfigList[m][n+1].TimeConfigId == 0)
            {
                stage_count = n+1;
                break;
            }
        }
        break;
    }
    total_stage_count_ = stage_count;
    str.sprintf("%d / %d", curr_stage_id_, total_stage_count_);
    stage_id_label_->setText(str);
    //:~ stage id
}

unsigned char SimulatorWidget::getPhaseType(unsigned char phase_id)
{
    unsigned phase_type = 0;
    for (int i = 0; i < tsc_param_.phase_table_.FactPhaseNum; i++)
    {
        if (phase_id == tsc_param_.phase_table_.PhaseList[i].PhaseId)
        {
            phase_type = tsc_param_.phase_table_.PhaseList[i].PhaseType;
            break;
        }
    }
    return phase_type;
}

bool SimulatorWidget::checkLaneId()
{
    QList<int> lane_detector_id_list = road_branch_widget_->getLaneDetectorIdList();
    QList<int> sidewalk_detector_id_list = road_branch_widget_->getSidewalkDetectorIdList();
    if (lane_detector_id_list.contains(0) || sidewalk_detector_id_list.contains(0))
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
    QList<int> detector_id_list = road_branch_widget_->getLaneDetectorIdList();
//    qDebug() << "detector_id_list:" << detector_id_list << endl
//             << "curr_lane_index:" << lane_index;
    curr_lane_id_ = detector_id_list.at(lane_index);
    if (curr_lane_id_ >= 1 && curr_lane_id_ <= 48)
    {
        com_data.type = 0x01 + '\0';
        need_leave_list_[lane_index] = true;
    }
    else if (curr_lane_id_ <= 56)
    {
        com_data.type = 0x04 + '\0';
        need_leave_list_[lane_index] = false;
    }
    else if (curr_lane_id_ <= 60)
    {
        com_data.type = 0x05 + '\0';
        need_leave_list_[lane_index] = false;
    }
    com_data.detector_id = curr_lane_id_ + '\0';
    int secs = QDateTime::currentDateTime().toTime_t();
    memcpy(com_data.ms_time, &secs, 4);

    com_array_.append(com_data.head);
    com_array_.append(com_data.type);
    com_array_.append(com_data.detector_id);
    com_array_.append(com_data.ms_time,2);
    com_array_.append(com_data.tail);
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
    else if (baud_rate == "115200")
    {
        my_com_setting_.BaudRate = BAUD115200;
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

void SimulatorWidget::initPreDetectorColorList()
{
    for (int i = 0; i < 16; i++)
    {
        pre_detector_color_list_.append(RoadBranchWidget::Red);
    }
}

void SimulatorWidget::initRedDetectorFlagList()
{
    for (int i = 0; i < 16; i++)
    {
        detector_red_flag_list_.append(0);
    }
}

bool SimulatorWidget::checkPackage(QByteArray &array)
{
    if (array.contains("DETECTDATAER"))
    {
        QMessageBox::information(this, STRING_TIP, STRING_UI_DETECTOR_RETURN_NULL, STRING_OK);
        int index = array.indexOf("DETECTDATAER");
        array.remove(index, QString("DETECTDATAER").size()+1);
        return false;
    }
    if (array.contains("DRIVEINFOER"))
    {
        QMessageBox::information(this, STRING_TIP, STRING_UI_DRIVER_RETURN_NULL, STRING_OK);
        int index = array.indexOf("DRIVEINFOER");
        array.remove(index, QString("DRIVEINFOER").size()+1);
        return false;
    }
    return true;
}

bool SimulatorWidget::parseConfigContent(QByteArray &array)
{
    if (array.isEmpty())
    {
        QMessageBox::information(this, STRING_TIP, STRING_UI_CONFIG_NULL, STRING_OK);
        return false;
    }
    QString tail(array.right(3));
    if (!(array.left(4).contains("CYT4") && array.right(3).contains("END")))
    {
        array.clear();
        return false;
    }
    array.remove(0,4);
    unsigned char temp[4] = {'\0'};
    memcpy(temp, array.data(), 4);
    unsigned int len = 0;
    memcpy(&len, temp, 4);
    array.remove(0,4);
    int idx = array.indexOf(tail);
    array.remove(idx, 3);
    len -= (4+3+4);
    if (len != (unsigned int)array.size())
    {
        array.clear();
        return false;
    }
    return true;
}

bool SimulatorWidget::parseBeginMonitorContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(idx, 3);
    if ((unsigned int)array.size() < sizeof(begin_monitor_info_))
    {
        array.remove(0,2);
        return false;
    }
    memcpy(&begin_monitor_info_, array.data(), sizeof(begin_monitor_info_));
    array.remove(0,2);
    // TODO: update lane light and sidewalk light
    int id = begin_monitor_info_.channel_id;
    // if id > 16, did not emit signal
    emit showLightSignal(id, Off);   // close pre-light
    emit showLightSignal(id, begin_monitor_info_.status);

    return true;
}

bool SimulatorWidget::parseLightStatusContent(QByteArray &array)
{
    array.remove(0,4);
    char array_sz = '\0';
    char num_1 = '\0';
    memcpy(&array_sz, array.data(), 1);
    array_sz -= '0';
    array.remove(0,1);
    memcpy(&num_1, array.data(), 1);
    RYGArray ryg;
    for (int i = 0; i < array_sz; i++)
    {
        array.remove(0,1);
        memcpy(&ryg, array.data(), 3);
        light_status_info_.lights[i] = ryg;
        array.remove(0,3);
    }
    memcpy(&light_status_info_.work_mode, array.data(), 1);
    array.remove(0,1);
    memcpy(&light_status_info_.plan_id, array.data(),1);
    array.remove(0,1);
    memcpy(&light_status_info_.phase_id, array.data(), 4);
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(idx, 3);

    channel_status_info_.work_mode = light_status_info_.work_mode;
    channel_status_info_.stage_id = light_status_info_.plan_id;
    channel_status_info_.phase_id = light_status_info_.phase_id;
    channel_status_info_.channel_vec.clear();
    unsigned char red_tmp = 0, yellow_tmp = 0, green_tmp = 0;
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 8; j++)
        {
            red_tmp = light_status_info_.lights[i].red;
            yellow_tmp = light_status_info_.lights[i].yellow;
            green_tmp = light_status_info_.lights[i].green;
            if ((red_tmp & (0x01 << j)) != 0x00)
            {
                channel_status_info_.channel_vec.append(Red);
            }
            else if ((yellow_tmp & (0x01 << j)) != 0x00)
            {
                channel_status_info_.channel_vec.append(Yellow);
            }
            else if ((green_tmp & (0x01 << j)) != 0x00)
            {
                channel_status_info_.channel_vec.append(Green);
            }
            else
            {
                channel_status_info_.channel_vec.append(Off);
            }
        }
    }

    // TODO: update ui
    for (int i = 0; i < channel_status_info_.channel_vec.size(); i++)
    {
        emit showLightSignal(i+1, channel_status_info_.channel_vec.at(i));
    }
    QString txt = ctrl_mode_desc_map_.value(channel_status_info_.work_mode);
    ctrl_mode_label_->setText(txt);

    // back up channel status info
    channel_status_bak_ = channel_status_info_;

    QString str;
    curr_stage_id_ = channel_status_bak_.stage_id;
    str.sprintf("%d / %d", curr_stage_id_, total_stage_count_);
    stage_id_label_->setText(str);
    str = phaseBitsDesc(channel_status_bak_.phase_id);
    curr_phase_id_label_->setText(str);

    return true;
}

bool SimulatorWidget::parseCountDownContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(idx, 3);
    memcpy(&count_down_info_, array.data(), sizeof(count_down_info_));
    array.remove(0,8);

    // TODO: update schedule info
    QString str;
    curr_stage_id_ = count_down_info_.stage_id;
    str.sprintf("%d / %d", curr_stage_id_, total_stage_count_);
    stage_id_label_->setText(str);
    curr_phase_id_label_->setText(phaseBitsDesc(count_down_info_.phase_ids));
    ctrl_mode_label_->setText(ctrl_mode_desc_map_.value(count_down_info_.ctrl_mode));

    count_down_secs_ = count_down_info_.light_time;
    count_down_light_ = count_down_info_.light_corlor;
    QString count_down_text("00-00-00");
    switch (count_down_info_.light_corlor)
    {
    case Red:
        count_down_text.sprintf("00-00-%02d", count_down_secs_);
        count_down_timer_->stop();
        count_down_lcd_->display(count_down_text);
        count_down_timer_->start(1000);
        break;
    case Yellow:
        count_down_text.sprintf("00-%02d-00", count_down_secs_);
        count_down_timer_->stop();
        count_down_lcd_->display(count_down_text);
        count_down_timer_->start(1000);
        break;
    case Green:
        count_down_text.sprintf("%02d-00-00", count_down_secs_);
        count_down_timer_->stop();
        count_down_lcd_->display(count_down_text);
        count_down_timer_->start(1000);
        break;
    case Off:
        count_down_text = "00-00-00";
        count_down_timer_->stop();
        count_down_lcd_->display(count_down_text);
        break;
    default:
        break;
    }
    return true;
}

bool SimulatorWidget::parseTSCTimeContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(idx, 3);
    if (array.size() < 4)
    {
        array.remove(0, array.size());
        return false;
    }
    unsigned char temp[4] = {'\0'};
    memcpy(temp, array.data(), 4);
    array.remove(0,4);
    unsigned int seconds = 0;
    memcpy(&seconds, temp, sizeof(seconds));
    if (seconds >= 60*60*8)
    {
        seconds -= 60*60*8;
    }
    date_time_ = QDateTime::fromTime_t(seconds);
    QString str = date_time_.toString("yyyy-MM-dd hh:mm:ss");
    SIGNALER_TIME_UPDATE(str);

    return true;
}

bool SimulatorWidget::parseAllLightOnContent(QByteArray &array)
{
    array.remove(0,4);
    unsigned char light_color = 0;
    memcpy(&light_color, array.data(), 1);
    int idx = array.indexOf("END");
    if (idx != 1)
    {
        array.remove(0, idx);
        return false;
    }
    array.remove(0,4);
    // TODO: update ui
    // set lane and sidewalk light the same color
    for (int i = 1; i < 16+1; i++)
    {
        emit showLightSignal(i, light_color);
    }

    return true;
}

bool SimulatorWidget::parseDetectorFlowContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(idx, 3);
    array.remove(0,4);
    return true;
}

bool SimulatorWidget::parseDetectorFaultContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(0, idx+3);
    return true;
}

bool SimulatorWidget::parseDriverStatusContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(0, idx+3);
    return true;
}

bool SimulatorWidget::parseRealTimeFlowContent(QByteArray &array)
{
    array.remove(0,4);
    array.remove(0,4);
    return true;
}

bool SimulatorWidget::parseDriverRealtimeStatusContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(0, idx+3);
    return true;
}

bool SimulatorWidget::parseLightRealTimeStatusContent(QByteArray &array)
{
    array.remove(0,4);
    int idx = array.indexOf("END");
    array.remove(0, idx+3);
    return true;
}
/* 处理流程：
 * 1. 检查当前所有车道上是否有车辆待发————有则发车，无则继续
 * 2. 获取当前放行相位ID和当前相位所控制的通道
 * 3. 检查当前系统中可通行通道上有无待放行的车辆：有则放行
 * 4. 产生随机通道号并放行。
*/
bool SimulatorWidget::simualtorComdataDispatcher()
{
    QString ctrl_mode = ctrl_mode_label_->text().trimmed();
    unsigned char phase_id = curr_phase_id_label_->text().toInt();
    unsigned char phase_type = getPhaseType(phase_id);
    if (ctrl_mode == STRING_CTRL_FULL_INDUCTION)
    {
        return trafficDispatch(phase_id);
    }
    else if (ctrl_mode == STRING_CTRL_MAIN_HALF_INDUCTION)
    {
        if (phase_type == 0x020)        // elasticity phase
        {
            // send com msg
            return trafficDispatch(phase_id);
        }
        else if (phase_type == 0x080)   // fix phase
        {
            // TODO: did not send com msg
            ;
        }
    }
    else if (ctrl_mode == STRING_CTRL_SECOND_HALF_INDUC)
    {
        if (phase_type == 0x020)    // elasticity phase
        {
            // TODO: did not send com msg
            ;
        }
        else if (phase_type == 0x040)   // determined phase
        {
            // TODO: send com msg
            return trafficDispatch(phase_id);
        }
    }
    else if (ctrl_mode == STRING_CTRL_CROSS_STREET)
    {
        if (phase_type == 0x01)     // motor phase
        {
            // TODO: did not send com msg
            ;
        }
        else if (phase_type == 0x04)    // walkman phase
        {
            // TODO: send com msg
            return trafficDispatch(phase_id);
        }
    }
    else if (ctrl_mode == STRING_CTRL_BUS_FIRST)
    {
        // TODO: send com msg
        return trafficDispatch(phase_id);
    }
    else if (ctrl_mode == STRING_CTRL_SINGLE_ADAPT)
    {
        // TODO: send com msg
        return trafficDispatch(phase_id);
    }
    else
    {
        return trafficDispatch(phase_id);
    }
    return false;
}

void SimulatorWidget::comDataDispatch(int phase_id)
{
    QList<unsigned char> channel_id_list = phase_handler_->get_phase_ctrled_channel_list(phase_id);/* road_branch_widget_->getLaneDetectorIdList();*/
    int sz = channel_id_list.size();
    if (sz <= 0) {return;}
    QTime t = QTime::currentTime();
    qsrand(t.msec() + t.second()*1000);
    int lane_idx = qrand() % 12;
    if (!channel_id_list.contains(lane_idx+1))
    {
        return;
    }
    qDebug() << "before pack com data (list_size:" << sz << "lane_idx:" << lane_idx << ")";
    packComData(lane_idx);
    qDebug() << "get_lane_index:" << lane_idx;
    for (int i = 0; i < channel_id_list.size(); i++)
    {
        if (detector_red_flag_list_.at(i) == 1)
        {
            packComData(i);
            updateDetectorStatus(lane_idx);
            detector_red_flag_list_[i] = 0;
        }
    }
    updateDetectorStatus(lane_idx);
}

void SimulatorWidget::updateDetectorStatus(int detector_index)
{
    if (need_leave_)
    {
        detector_red_flag_list_[detector_index] = 1;
    }
    else
    {
        detector_red_flag_list_[detector_index] = 0;
    }
    my_com_->write(com_array_);
    if (detector_red_flag_list_.at(pre_lane_idx_) == 0)
    {
        emit showLaneDetectorSignal(pre_lane_idx_, RoadBranchWidget::Green, false);
    }
    else
    {
        emit showLaneDetectorSignal(curr_lane_idx_, RoadBranchWidget::Red, false);
    }
    emit showLaneDetectorSignal(detector_index, RoadBranchWidget::Green, true);
    pre_lane_idx_ = detector_index;
    curr_lane_idx_ = detector_index;
}

QString SimulatorWidget::phaseBitsDesc(unsigned int phase_ids)
{
    QString str;
    for (int i = 1; i <= 32; i++)
    {
        if ((phase_ids & 0x01) == 0x01)
        {
            str += QString::number(i) + "/";
        }
        phase_ids = phase_ids >> 1;
    }
    if (str.isEmpty())
    {
        return "-";
    }
    return str.left(str.size() - 1);
}

bool SimulatorWidget::trafficDispatch(unsigned int phase_id)
{
    qDebug() << "dispatch-car_sent_list:";
    dumpEnumElementList(car_sent_list_);
    for (int i = 0; i < 12; i++)
    {
        int pre_index = pre_lane_idx_list_.at(i);
        LightColor color = Off;//channel_detector_color_list_.at(i);
        if (Q_LIKELY(pre_index >= 0))
        {
            color = channel_detector_color_list_.at(pre_index);
        }
        switch (car_sent_list_.at(i))
        {
        case Come:
            if (isChannelAccessible(phase_id, i+1))
            {
                // TODO: dispatch car
                if (pre_index >= 0 && color != Off)
                {
                    emit showLaneDetectorSignal(pre_index, color, false);
                    qDebug() << "dispatch-Come--close pre lane detector:" << pre_index+1 << ",Color:" << colorPrintable(color) << "false";
                }
                emit showLaneDetectorSignal(i, RoadBranchWidget::Green, true);
                qDebug() << "dispatch-Come--light lane detector:" << i+1 << ",Color: Green true";
                packComData(i);
                my_com_->write(com_array_);
                txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
                channel_detector_color_list_[i] = Green;
                car_sent_list_[i] = need_leave_list_.at(i) ? Go : None;
                pre_lane_idx_list_[i] = i;
            }
            break;
        case Go:
            if (isChannelAccessible(phase_id, i+1))
            {
                // TODO: dispatch car
                if (pre_index >= 0)
                {
                    emit showLaneDetectorSignal(pre_index, color, false);
                    qDebug() << "Go--close detector:(lane id:" << pre_index+1 << ",Color:" << colorPrintable(color) << ") false";
                }
                emit showLaneDetectorSignal(i, RoadBranchWidget::Red, true);
                qDebug() << "Go--show detector:(lane id:" << i+1 << ",Color:Red) true";
                if (need_leave_list_.at(i))
                {
                    packComData(i);
                    com_array_[1] = 0x02 + '\0';
                    char ms[4] = {'\0'};
                    int secs = QDateTime::currentDateTime().toTime_t();
                    memcpy(ms, &secs, sizeof(secs));
                    com_array_[3] = ms[0];
                    com_array_[4] = ms[1];
                    need_leave_list_[i] = false;
                }
                else
                {
                    packComData(i);
                }
                my_com_->write(com_array_);
                txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
                need_leave_list_[i] = false;
                channel_detector_color_list_[i] = Red;
                car_sent_list_[i] = None;
                pre_lane_idx_list_[i] = i;
            }
            break;
        case None:
            if (pre_index >= 0 && color != Off)
            {
                emit showLaneDetectorSignal(pre_index, color, false);
                car_sent_list_[pre_index] = Other;
                qDebug() << "None--close detector:" << pre_index+1 << ",Color:" << colorPrintable(color) << "false";
            }
            break;
        case Other:
            break;
        default:
            break;
        }
    }
    // generate rand lane index
    QTime t = QTime::currentTime();
    qsrand(t.msec() + t.second()*1000);
    int lane_idx = qrand() % 12;
    qDebug() << "rand lane id:" << lane_idx+1;
    if (car_sent_list_.at(lane_idx) == Go)
    {
        qDebug() << "lane" << lane_idx+1 << " exists a car (Green)";
        return false;
    }
    car_sent_list_[lane_idx] = Come;
    packComData(lane_idx);
    switch (car_sent_list_.at(lane_idx))
    {
    case Come:
        if (isChannelAccessible(phase_id, lane_idx+1))
        {
            road_branch_widget_->showDetectorSlot(lane_idx, RoadBranchWidget::Green, true);
            qDebug() << "Come--show detector:(lane id:" << lane_idx+1 << ",Color: Green) true";
            channel_detector_color_list_[lane_idx] = Green;
            car_sent_list_[lane_idx] = need_leave_list_.at(lane_idx) ? Go : None;
            pre_lane_idx_list_[lane_idx] = lane_idx;
            txt_edit_->insertPlainText(formatComData(com_array_)+"\n");
            my_com_->write(com_array_);
            if (need_leave_list_.at(lane_idx))
            {
                timer_->start(qrand()%1000+1000);
            }
        }
        else
        {
            qDebug() << "car on lane id:" << lane_idx+1 << "can not access this channel";
            car_sent_list_[lane_idx] = Other;
        }
        break;
    case Go:
        break;
    case None:
        break;
    default:
        break;
    }
    return true;
}

bool SimulatorWidget::isChannelAccessible(unsigned int phase_id, unsigned char channel_id)
{
    QList<unsigned char> channel_id_list = phase_handler_->get_phase_ctrled_channel_list(phase_id);
    if (channel_id_list.isEmpty())
    {
        return false;
    }
    if (!channel_id_list.contains(channel_id))
    {
        return false;
    }
    return true;
}

void SimulatorWidget::randTraffic()
{
    QTime t = QTime::currentTime();
    qsrand(t.msec() + t.second()*1000);
    int lane_idx = qrand() % 12;
//    qDebug() << "rand lane_idx:" << lane_idx;
    packComData(lane_idx);
    my_com_->write(com_array_);
}

void SimulatorWidget::initTrafficDispatcher()
{
    for (int i = 0; i < 16; i++)
    {
        car_sent_list_.append(Other);
        channel_detector_color_list_.append(Off);
        pre_lane_idx_list_.append(-1);
        need_leave_list_.append(false);
    }
}

void SimulatorWidget::dumpComData()
{
    SerialData com_data;
    com_data.type = com_array_.at(1);
    com_data.detector_id = com_array_.at(2);
    com_data.ms_time[0] = com_array_.at(3);
    com_data.ms_time[1] = com_array_.at(4);

    int ts = 0;
    memcpy(&ts, com_data.ms_time,4);
    QString head = QString("0x%1").arg(int(com_data.head),0,16);
    QString tail = QString("0x%1").arg(int(com_data.tail),0,16);
    qDebug() << "head:" << head
             << "type:" << com_data.type - '\0'
             << "lane_id:" << com_data.detector_id - '\0'
             << "timespan:" << ts
             << "tail:" << tail
             << endl;
}

void SimulatorWidget::test()
{
    QFile file("test.dat");
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    file.write(com_array_);
    file.close();
}

QString SimulatorWidget::colorPrintable(SimulatorWidget::LightColor color)
{
    QString str;
    switch(color)
    {
    case Red:
        str = "Red";
        break;
    case Yellow:
        str = "Yellow";
        break;
    case Green:
        str = "Green";
        break;
    case Off:
        str = "Off";
        break;
    case Invalid:
        str = "Invalid";
        break;
    default:
        str = "-";
        break;
    }
    return str;
}

QString SimulatorWidget::carStatusPrintable(SimulatorWidget::CarStatus status)
{
    QString str;
    switch (status)
    {
    case Come:
        str = "Come";
        break;
    case Go:
        str = "Go";
        break;
    case None:
        str = "None";
        break;
    case Other:
        str = "Other";
        break;
    default:
        str = "-";
        break;
    }
    return str;
}

void SimulatorWidget::dumpEnumElementList(const QList<SimulatorWidget::CarStatus> &list)
{
    int cnt = list.size();
    if (cnt <= 0)
    {
        return;
    }
    QString str;
    for (int i = 0; i < cnt; i++)
    {
        str += QString::number(i+1) + "-" + carStatusPrintable(list.at(i)) + " ";
    }
    qDebug() << str;
}

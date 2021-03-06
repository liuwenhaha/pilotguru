#include "steering_nn_main_window.h"

#include <io/kia_json_loggers.hpp>
#include <ui_steering_nn_main_window.h>

#include <spoof-steering-serial-commands.h>

#include <iostream>

using pilotguru::kia::KiaControlCommand;
using pilotguru::kia::SteeringAngle;

SteeringNNMainWindow::SteeringNNMainWindow(
    const std::string &can_interface, const std::string &arduino_tty,
    const pilotguru::kia::SteeringAngleHolderSettings
        &steering_controller_settings,
    zmq::socket_t *prediction_data_socket, const std::string &log_dir,
    QWidget *parent)
    : QMainWindow(parent), ui(new Ui::SteeringNNMainWindow),
      car_motion_data_(new pilotguru::kia::CarMotionData(10)),
      car_motion_data_updater_(new pilotguru::kia::CarMotionDataUpdater(
          car_motion_data_.get(), can_interface, {0x2B0, 0x4B0}, {1, 0})),
      arduino_command_channel_(
          new pilotguru::ArduinoCommandChannel(arduino_tty)),
      steering_controller_(new pilotguru::kia::SteeringAngleHolderController(
          &(car_motion_data_->steering_angles()),
          arduino_command_channel_.get(), steering_controller_settings)),
      prediction_updater_(new pilotguru::SingleSteeringAnglePredictionUpdater(
          CHECK_NOTNULL(prediction_data_socket), 5 /* history length */)),
      steering_controller_feeder_(new pilotguru::kia::SteeringAngleHolderFeeder(
          steering_controller_.get(), &(prediction_updater_->Predictions()),
          true /* clip_target_angle */)),
      kia_commands_logger_(
          new pilotguru::TimestampedJsonLogger<KiaControlCommand>(
              log_dir, pilotguru::STEERING_COMMANDS_LOG_ROOT_ELEMENT,
              std::unique_ptr<pilotguru::JsonSteamWriter<KiaControlCommand>>(
                  new pilotguru::SteeringCommandsJsonWriter()),
              &(arduino_command_channel_->CommandsHistory()))),
      steering_angles_logger_(
          new pilotguru::TimestampedJsonLogger<SteeringAngle>(
              log_dir, pilotguru::STEERING_ANGLES_LOG_ROOT_ELEMENT,
              std::unique_ptr<pilotguru::JsonSteamWriter<SteeringAngle>>(
                  new pilotguru::SteeringAngleJsonWriter()),
              &(car_motion_data_->steering_angles()))),
      target_steering_angles_logger_(new pilotguru::TimestampedJsonLogger<
                                     pilotguru::kia::TargetSteeringAngleStatus>(
          log_dir, pilotguru::TARGET_STEERING_ANGLES_LOG_ROOT_ELEMENT,
          std::unique_ptr<pilotguru::JsonSteamWriter<
              pilotguru::kia::TargetSteeringAngleStatus>>(
              new pilotguru::TargetSteeringAngleStatusJsonWriter()),
          &(steering_controller_->TargetSteeringAnglesHistory()))) {
  ui->setupUi(this);

  prediction_updater_->Start();
  steering_controller_feeder_->Start();

  steering_angle_read_thread_.reset(
      new SteeringAngleReadThread(&(car_motion_data_->steering_angles())));
  connect(steering_angle_read_thread_.get(),
          &SteeringAngleReadThread::SteeringAngleChanged, this,
          &SteeringNNMainWindow::OnSteeringAngleChanged);
  steering_angle_read_thread_->start();

  velocity_read_thread_.reset(
      new VelocityReadThread(&(car_motion_data_->velocities())));
  connect(velocity_read_thread_.get(), &VelocityReadThread::VelocityChanged,
          this, &SteeringNNMainWindow::OnVelocityChanged);
  velocity_read_thread_->start();

  steering_torque_offset_read_thread_.reset(new SteeringTorqueOffsetReadThread(
      &(arduino_command_channel_->CommandsHistory())));
  connect(steering_torque_offset_read_thread_.get(),
          &SteeringTorqueOffsetReadThread::SteeringTorqueChanged, this,
          &SteeringNNMainWindow::OnSteeringTorqueChanged);
  steering_torque_offset_read_thread_->start();

  // TODO separate indicators for incoming predictions and effective controller
  // target angles?
  steering_prediction_read_thread_.reset(new SteeringPredictionReadThread(
      &(steering_controller_->TargetSteeringAnglesHistory())));
  connect(steering_prediction_read_thread_.get(),
          &SteeringPredictionReadThread::SteeringPredictionChanged, this,
          &SteeringNNMainWindow::OnSteeringPredictionChanged);
  steering_prediction_read_thread_->start();

  connect(ui->predictor_start_button, &QPushButton::clicked, this,
          &SteeringNNMainWindow::PredictionUpdaterStart);
  connect(ui->predictor_stop_button, &QPushButton::clicked, this,
          &SteeringNNMainWindow::PredictionUpdaterStop);
  connect(ui->steering_start_button, &QPushButton::clicked, this,
          &SteeringNNMainWindow::SteeringStart);
  connect(ui->steering_stop_button, &QPushButton::clicked, this,
          &SteeringNNMainWindow::SteeringStop);
}

SteeringNNMainWindow::~SteeringNNMainWindow() {
  steering_angle_read_thread_->RequestStop();
  velocity_read_thread_->RequestStop();
  steering_torque_offset_read_thread_->RequestStop();
  steering_prediction_read_thread_->RequestStop();
  steering_angle_read_thread_->wait();
  velocity_read_thread_->wait();
  steering_torque_offset_read_thread_->wait();
  steering_prediction_read_thread_->wait();
  kia_commands_logger_->Stop();
  steering_angles_logger_->Stop();
  target_steering_angles_logger_->Stop();
  steering_controller_->Stop();
  car_motion_data_updater_->stop();
  prediction_updater_->Stop();
  steering_controller_feeder_->Stop();
  delete ui;
}

void SteeringNNMainWindow::OnSteeringAngleChanged(int16_t angle_deci_degrees) {
  const double angle_degrees = static_cast<double>(angle_deci_degrees) / 10.0;
  ui->steering_angle_value_label->setText(QString::number(angle_degrees));
}

void SteeringNNMainWindow::OnVelocityChanged(QString text) {
  ui->velocity_value_label->setText(text);
}

void SteeringNNMainWindow::OnSteeringTorqueChanged(QString text) {
  ui->torque_offset_value_label->setText(text);
}

void SteeringNNMainWindow::OnSteeringPredictionChanged(QString text) {
  ui->target_angle_value_label->setText(text);
}

void SteeringNNMainWindow::PredictionUpdaterStart() {
  // TODO add signal to the predictor module.
  steering_controller_feeder_->SetFeedEnabled(true);
}

void SteeringNNMainWindow::PredictionUpdaterStop() {
  // TODO add signal to the predictor module.
  steering_controller_feeder_->SetFeedEnabled(false);
}

void SteeringNNMainWindow::SteeringStart() {
  car_motion_data_updater_->start();
}

void SteeringNNMainWindow::SteeringStop() { car_motion_data_updater_->stop(); }

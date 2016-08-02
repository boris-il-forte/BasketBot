//-----------------//
// DC right module //
//-----------------//

#include <Configuration.hpp>
#include <Module.hpp>

// --- MESSAGES ---------------------------------------------------------------
#include <common_msgs/Led.hpp>
#include <actuator_msgs/Setpoint_f32.hpp>

// --- NODES ------------------------------------------------------------------
#include <sensor_publisher/Publisher.hpp>
#include <actuator_subscriber/Subscriber.hpp>
#include <led/Subscriber.hpp>

// --- BOARD IMPL -------------------------------------------------------------
#include <QEI_driver/QEI.hpp>
#include <A4957_driver/A4957.hpp>
#include <current_control/CurrentPID.hpp>
#include <current_control/Calibration.hpp>

// *** DO NOT MOVE ***
Module module;

// --- TYPES ------------------------------------------------------------------
using QEI_Publisher = sensor_publisher::Publisher<Configuration::QEI_DELTA_DATATYPE>;
using CurrentSensor = current_control::CurrentSensor;
using CurrentPID = current_control::CurrentPID;
using Calibration = current_control::Calibration;

// --- NODES ------------------------------------------------------------------

//#define CALIBRATION
led::Subscriber led_subscriber("led_subscriber", Core::MW::Thread::PriorityEnum::LOWEST);

#ifdef CALIBRATION
Calibration calibration("calibration", module.hbridge_pwm, Core::MW::Thread::PriorityEnum::NORMAL);
#else
QEI_Publisher encoder("encoder", module.qei, Core::MW::Thread::PriorityEnum::NORMAL);
CurrentSensor currentSensor; //TODO move in module
CurrentPID currentPid("current_pid", currentSensor, module.hbridge_pwm, Core::MW::Thread::PriorityEnum::NORMAL);
#endif




// --- MAIN -------------------------------------------------------------------
extern "C" {
   int
   main()
   {
      module.initialize();

      currentSensor.init(); //TODO move in module

      // Module configuration
      module.qei.configuration["period"] = 50;
      module.qei.configuration["ticks"] = 1000;

      // Nodes configuration
      led_subscriber.configuration["topic"] = "led";

#ifndef CALIBRATION
      encoder.configuration["topic"] = "encoder_right";

      currentSensor.configuration.a = 0.007432946790511f;
      currentSensor.configuration.b = -15.107809133385506f;

      currentPid.configuration.maxV = 24;
      currentPid.configuration.R = 0.299f;
      currentPid.configuration.L = 8.2e-5f;
      currentPid.configuration.T = 0.0115f;
      currentPid.configuration.Kt = 30.2e-3;

      currentPid.configuration.controlCycles = 1;
      currentPid.configuration.omegaC = 6000.0f;
      currentPid.configuration.topic = "torque_right";
#endif

      // Add nodes to the node manager (== board)...
      module.add(led_subscriber);

#ifdef CALIBRATION
      module.add(calibration);
#else
      module.add(encoder);
      module.add(currentPid);
#endif

      // ... and let's play!
      module.setup();
      module.run();

      // Is everything going well?
      for (;;) {
         if (!module.isOk()) {
            module.halt("This must not happen!");
         }

         Core::MW::Thread::sleep(Core::MW::Time::ms(500));
      }

      return Core::MW::Thread::OK;
   } // main
}

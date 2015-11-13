#include "ch.h"
#include "hal.h"

#include "r2p/Middleware.hpp"

#include "current_pid_node.hpp"

#include <r2p/node/pid.hpp>

namespace r2p {

/*===========================================================================*/
/* Macro definitions.                                                        */
/*===========================================================================*/

#define ABS(x) ((x) >= 0) ? (x) : -(x)

/*===========================================================================*/
/* Motor parameters.                                                         */
/*===========================================================================*/
#define ADC_NUM_CHANNELS   1
#define ADC_BUF_DEPTH      1

#define _Ts                (1.0f/17.5e3)
#define _pwmTicks          4095.0f
#define _pwmMin            200
#define _controlCycles     1

static PID current_pid;
static float currentPeak = 0.0f;
static float current = 0.0f;
static float measure = 0.0f;

static int Kpwm;
static int pwm = 0;
static int controlCounter = 0;

/*===========================================================================*/
/* Current sensor parameters.                                                */
/*===========================================================================*/

#define _Kcs               -0.007771336616934f
#define _Qcs               15.847698196081865f

/*===========================================================================*/
/* Config adc and pwm                                                        */
/*===========================================================================*/

static adcsample_t adc_samples[ADC_NUM_CHANNELS * ADC_BUF_DEPTH];

static void current_callback(ADCDriver *adcp, adcsample_t *buffer, size_t n) {

	(void) adcp;
	(void) n;

	palTogglePad(LED2_GPIO, LED2);

	//Compute current
	chSysLockFromIsr()
	currentPeak = (_Kcs * buffer[0] + _Qcs);
	chSysUnlockFromIsr();

	palTogglePad(LED2_GPIO, LED2);

}

static void control_callback(PWMDriver *pwmp) {
	(void) pwmp;

	palTogglePad(LED1_GPIO, LED1);

	//Add new current peak
	current += currentPeak;

	//Count cycle
	controlCounter++;

	//compute control if control cycle
	if (controlCounter == _controlCycles) {

		// Compute mean current
		current *= ABS(pwm) / _controlCycles;

		chSysLockFromIsr()

		// Compute control
		float voltage = current_pid.update(current);

		//Compute pwm signal
		pwm = voltage / Kpwm;

		//Set pwm to 0 if not in controllable region
		int dutyCycle = ABS(pwm);
		if (dutyCycle <= _pwmMin) {
			currentPeak = 0;
			pwm = 0;
			dutyCycle = 0;
		}

		chSysUnlockFromIsr();

		palTogglePad(LED1_GPIO, LED1);

		pwm_lld_enable_channel(&PWM_DRIVER, pwm > 0 ? 1 : 0, dutyCycle);
		pwm_lld_enable_channel(&PWM_DRIVER, pwm > 0 ? 0 : 1, 0);

		pwm_lld_enable_channel(&PWM_DRIVER, 2, dutyCycle / 2 - 70);

		palTogglePad(LED1_GPIO, LED1);

		//set measure
		measure = current;

		// reset variables
		current = 0;
		controlCounter = 0;
	}

	palSetPad(LED1_GPIO, LED1);
}

/*
 * ADC conversion group.
 * Mode:        Circular buffer, 1 sample of 1 channel, triggered by pwm channel 3
 * Channels:    IN10.
 */
static const ADCConversionGroup adcgrpcfg = { TRUE, // circular
		ADC_NUM_CHANNELS, // num channels
		current_callback, // end callback
		NULL, // error callback
		0, // CR1
		ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL_1, // CR2
		0, // SMPR1
		ADC_SMPR2_SMP_AN3(ADC_SAMPLE_1P5), // SMPR2
		ADC_SQR1_NUM_CH(ADC_NUM_CHANNELS), // SQR1
		0, // SQR2
		ADC_SQR3_SQ1_N(ADC_CHANNEL_IN3) // SQR3
		};

static PWMConfig pwmcfg = { STM32_SYSCLK, // 72MHz PWM clock frequency.
		4096, // 12-bit PWM, 17KHz frequency.
		control_callback, // pwm callback
		{ { PWM_OUTPUT_ACTIVE_HIGH | PWM_COMPLEMENTARY_OUTPUT_ACTIVE_HIGH,
		NULL }, //
				{ PWM_OUTPUT_ACTIVE_HIGH | PWM_COMPLEMENTARY_OUTPUT_ACTIVE_HIGH,
				NULL }, //
				{ PWM_OUTPUT_ACTIVE_LOW, NULL }, //
				{ PWM_OUTPUT_DISABLED, NULL } }, //
		0, //
#if STM32_PWM_USE_ADVANCED
		72, /* XXX 1uS deadtime insertion   */
#endif
		0 };

/*===========================================================================*/
/* Motor control nodes.                                                      */
/*===========================================================================*/

/*
 * PID node.
 */

static current_pid_node_conf defaultConf = { "current_pid", "current_measure",
		0, 0.110f, 2.5e-5f, 6000.0f, 24.0f };

msg_t current_pid2_node(void * arg) {
	//Configure current node
	current_pid_node_conf* conf;
	if (arg != NULL)
		conf = (current_pid_node_conf *) arg;
	else
		conf = &defaultConf;

	Node node(conf->name);
	Publisher<CurrentMsg> current_pub;
	Subscriber<Current2Msg, 5> current_sub;
	Current2Msg * msgp_in;
	CurrentMsg * msgp_out;

	Time last_setpoint(0);

	chRegSetThreadName(conf->name);

	int index = conf->index;
	const float Kp = conf->omegaC * conf->L;
	const float Ti = conf->L / conf->R;
	Kpwm = conf->maxV;
	current_pid.config(Kp, Ti, 0.0, _Ts, -conf->maxV * _pwmTicks,
			conf->maxV * _pwmTicks);

	// Subscribe and publish topics
	node.subscribe(current_sub, "current2");
	node.advertise(current_pub, conf->topic);

	//set pid setpoint
	current_pid.set(0.0);

	// Start the ADC driver and conversion
	adcStart(&ADC_DRIVER, NULL);
	adcStartConversion(&ADC_DRIVER, &adcgrpcfg, adc_samples, ADC_BUF_DEPTH);

	// Init motor driver
	palSetPad(DRIVER_GPIO, DRIVER_RESET);
	chThdSleepMilliseconds(500);
	pwmStart(&PWM_DRIVER, &pwmcfg);

	// comunication cycle
	for (;;) {

		//get time
		systime_t time = chTimeNow();

		// update setpoint
		if (current_sub.fetch(msgp_in)) {
			chSysLock()
			current_pid.set(msgp_in->value[index] * _pwmTicks);
			chSysUnlock();
			last_setpoint = Time::now();
			current_sub.release(*msgp_in);

			palTogglePad(LED3_GPIO, LED3);

		} else if (Time::now() - last_setpoint > Time::ms(100)) {
			chSysLock()
			current_pid.set(0.0);
			chSysUnlock();
			palTogglePad(LED4_GPIO, LED4);
		}

		// publish current
		if (current_pub.alloc(msgp_out)) {
			chSysLock()
			msgp_out->value = measure / _pwmTicks;
			chSysUnlock();
			current_pub.publish(*msgp_out);
		}

		time += US2ST(57);
		chThdSleepUntil(time);

	}

	return CH_SUCCESS;
}

} /* namespace r2p */

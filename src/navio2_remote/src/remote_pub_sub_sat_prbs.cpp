#include "RCInput.h"
#include "PWM.h"
#include "Util.h"
#include <unistd.h>

#include "ros/ros.h"
#include "sensor_msgs/Temperature.h"
#include <sstream>

#define MOTOR_PWM_OUT 9
#define SERVO_PWM_OUT 0
#define PRBS_FREQ 25 //frequency of the prbs signal : 8 bit => min = 25/8 max = 25/1 

int main(int argc, char **argv)
{
	ROS_INFO("Start");
	int saturation = 2000;

	int prbs_val = 0; //default prbs signal
	int freq = 100;

	ROS_INFO("number of argc %d", argc);

	if(argc == 1)
	{
		//case with default params
	}
	else if(argc == 2)
	{
		//case with only prbs value
		prbs_val = atoi(argv[1]);
		if(prbs_val > 500 || prbs_val < 0)
		{
			ROS_INFO("prbs val must be between 0 and 500");
			return 0;
		}
	}
	else if(argc == 3)
	{
		//case with prbs and freq

		prbs_val = atoi(argv[1]);
		if(prbs_val > 500 || prbs_val < 0)
		{
			ROS_INFO("prbs val must be between 0 and 500");
			return 0;
		}
	
		if(atoi(argv[2]) > 0 )
			freq = atoi(argv[2]);
		else
		{
			ROS_INFO("Frequency must be more than 0");
			return 0;
		}
	}
	else if(argc == 4)
	{
		//case with prbs and freq saturation

		prbs_val = atoi(argv[1]);
		if(prbs_val > 500 || prbs_val < 0)
		{
			ROS_INFO("prbs val must be between 0 and 500");
			return 0;
		}

		if(atoi(argv[2]) > 0 )
			freq = atoi(argv[2]);
		else
		{
			ROS_INFO("Frequency must be more than 0");
			return 0;
		}
	
		if(atoi(argv[3]) > 2000) saturation = 2000;
		else saturation = atoi(argv[3]);
	}
	else
	{
		ROS_INFO("not enough arguments ! Specify prbs value and throttle saturation.");
		return 0;
	}

	ROS_INFO("Beginning with prbs : %d frequency %d, and saturation  : %d", prbs_val, freq, saturation);


 	/***********************/
	/* Initialize The Node */
	/***********************/
	ros::init(argc, argv, "remote_reading_handler");
	ros::NodeHandle n;
	//ros::Publisher remote_pub = n.advertise<std_msgs::Float64MultiArray>("remote_readings", 1000);
	ros::Publisher remote_pub = n.advertise<sensor_msgs::Temperature>("remote_readings", 1000);
	
	//running rate = freq Hz
	ros::Rate loop_rate(freq);

	/*******************************************/
	/* Initialize the RC input, and PWM output */
	/*******************************************/

	RCInput rcin;
	rcin.init();
	PWM servo;
	PWM motor;

	if (!motor.init(MOTOR_PWM_OUT)) {
		fprintf(stderr, "Motor Output Enable not set. Are you root?\n");
		return 0;
    	}

	if (!servo.init(SERVO_PWM_OUT)) {
		fprintf(stderr, "Servo Output Enable not set. Are you root?\n");
		return 0;
    	}

	motor.enable(MOTOR_PWM_OUT);
	servo.enable(SERVO_PWM_OUT);

	motor.set_period(MOTOR_PWM_OUT, 50); //frequency 50Hz
	servo.set_period(SERVO_PWM_OUT, 50); 

	int motor_input = 0;
	int servo_input = 0;

	sensor_msgs::Temperature rem_msg;

	//prbs start state
	int start_state = 0x7D0;
	int lfsr = start_state;

	int steer_low = 1500 - prbs_val;
	int steer_high= 1500 + prbs_val;
	int steer_prbs = steer_low;

	int ctr = 0; //counter for the period divider 
	while (ros::ok())
	{
		ctr %= freq/PRBS_FREQ;

		//Throttle saturation
		if(rcin.read(3) >= saturation)
			motor_input = saturation;
		else
			motor_input = rcin.read(3);

		//servo control with prbs
		servo_input = rcin.read(2);
		if(!ctr)
		{
			int bit = ((lfsr >> 0) ^ (lfsr >> 2)) & 1;
			lfsr = (lfsr >> 1) | (bit << 8); //was bit << 10 before

			if (bit == 1)
				steer_prbs = steer_high;
			else if (bit == 0)
				steer_prbs = steer_low;
			else
				steer_prbs = servo_input;
		}
		ctr++;
		if (servo_input > steer_high || servo_input < steer_low)
			servo_input = servo_input;
		else
			servo_input = steer_prbs;
		
		//write readings on pwm output
		motor.set_duty_cycle(MOTOR_PWM_OUT, ((float)motor_input)/1000.0f); 
		servo.set_duty_cycle(SERVO_PWM_OUT, ((float)servo_input)/1000.0f);
		
		//save values into msg container a
		rem_msg.header.stamp = ros::Time::now();
		rem_msg.temperature = motor_input;
		rem_msg.variance = servo_input;

		//debug info
		ROS_INFO("Thrust usec = %d    ---   Steering usec = %d", motor_input, servo_input);

		//remote_pub.publish(apub);
		remote_pub.publish(rem_msg);
		
		ros::spinOnce();

		loop_rate.sleep();

	}


  	return 0;
}


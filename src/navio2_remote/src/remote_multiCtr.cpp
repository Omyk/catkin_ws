#include "RCInput.h"
#include "PWM.h"
#include "Util.h"
#include <unistd.h>

#include "ros/ros.h"
#include "sensor_msgs/Temperature.h"
#include "sensor_msgs/Imu.h"
#include <sstream>

//PWM Pins on Navio2
#define MOTOR_PWM_OUT 9
#define SERVO_PWM_OUT 0

//Maximum Integration angle
#define MAX_IERR1 4
#define MAX_IERR2 5
#define PI 3.14159
#define SERVO_TRIM 1440.0f

// PID for roll angle
#define Kp1 0.3f
#define Ki1 0.0f
#define Kd1 0.03f

// PID for roll angle outer loop
float Kp2[3] = {0, 1.05f, 1.22f};
float Ki2[3] = {2.23f, 5.89f, 5.83f};
float Kd2[3] = {0, 0.0465f, 0.0195f};

//full range of motor
#define MAX_IERR_MOTOR 20.6

float max_roll_angle = 10.0f;

float currentRoll;
ros::Time currentTime;
ros::Time previousTime;

float currentSpeed;
ros::Time currentTimeSpeed;
ros::Time previousTimeSpeed;

//Roll Errors 1
float err1;
float derr1;
float Kierr1;

//Roll Errors 2
float err2;
float derr2;
float Kierr2;

//Motor Errors
float err_m;
float derr_m;
float Kierr_m;

//Motor PID parameters to pass
float Kp_m;
float Ki_m;
float Kd_m;

float RollOffset = 0; // offset to add to roll measurement for initial calibration

int the_time = 0;

//this function outputs the outer loop controller roll reference angle
float pid_Ref_Output(int desired_roll) //in degrees
{
	int idx = 0;
	if(currentSpeed < 4.5f) {idx = 0; max_roll_angle = 10.0f;}
	else if(currentSpeed < 5.5f && currentSpeed >= 4.5f) {idx = 1; max_roll_angle = 20.0f;}
	else {idx = 2; max_roll_angle = 30.0f;}
	
	//calculate errors
	float previousErr = err2;
	err2 = desired_roll - currentRoll;

	long timeNow = currentTime.nsec;

	//time between now and last roll message we got
	double dTnsec = (timeNow - previousTime.nsec); // in nanoseconds
	if(dTnsec < 0) dTnsec += 1e9; // watch out cause its in ns so if it goes beyond 1 sec ...
	double dT = dTnsec/(1e9f);

	if(dT > 0)
		derr2 = (err2 - previousErr)/dT;

	Kierr2 += Ki2[idx]*err2*dT;

	//anti wind-up (saturation)
	if(Kierr2 > MAX_IERR2) Kierr2 = MAX_IERR2;
	if(Kierr2 < -MAX_IERR2) Kierr2 = -MAX_IERR2;
	
	//PID CONTROLLER
	float controlSignal = Kp2[idx]*err2 + Kierr2 + Kd2[idx]*derr2; // should be between +- 30 deg (roll limit)
	if(controlSignal > max_roll_angle) controlSignal = max_roll_angle;
	if(controlSignal < -max_roll_angle) controlSignal = -max_roll_angle;

	return controlSignal; 
}

int pid_Servo_Output(int desired_roll) //in degrees
{
	//calculate errors
	float previousErr = err1;
	err1 = desired_roll - currentRoll;

	long timeNow = currentTime.nsec;

	//time between now and last roll message we got
	double dTnsec = (timeNow - previousTime.nsec); // in nanoseconds
	if(dTnsec < 0) dTnsec += 1e9; // watch out cause its in ns so if it goes beyond 1 sec ...
	double dT = dTnsec/(1e9f);

	if(dT > 0)
		derr1 = (err1 - previousErr)/dT;

	Kierr1 += Ki1*err1*dT;

	//anti wind-up (saturation)
	if(Kierr1 > MAX_IERR1) Kierr1 = MAX_IERR1;
	if(Kierr1 < -MAX_IERR1) Kierr1 = -MAX_IERR1;
	
	//PID CONTROLLER
	float controlSignal = Kp1*err1 + Kierr1 + Kd1*derr1; // should be between +- 22 deg (steer limit)
	
	int pwmSignal = (int)((-controlSignal*250.0f)/22.0f)+(SERVO_TRIM);
	if(pwmSignal > 1750) pwmSignal = 1750;
	if(pwmSignal < 1250) pwmSignal = 1250;

	return pwmSignal; 
}

int pid_Motor_Output(int desired_speed) // desired speed in m/s
{
	//calculate errors
	float previousErr = err_m;
	err_m = desired_speed - currentSpeed;

	long timeNow = currentTimeSpeed.nsec;

	//time between now and last roll message we got
	double dTnsec = (timeNow - previousTimeSpeed.nsec); // in nanoseconds
	if(dTnsec < 0) dTnsec += 1e9; // watch out cause its in ns so if it goes beyond 1 sec ...
	double dT = dTnsec/(1e9f);

	if(dT > 0)
		derr_m = (err_m - previousErr)/dT;

	Kierr_m += Ki_m*err_m*dT;

	//old anti wind-up (saturation)
	if(Kierr_m > MAX_IERR_MOTOR) Kierr_m = MAX_IERR_MOTOR;
	if(Kierr_m < -MAX_IERR_MOTOR) Kierr_m = -MAX_IERR_MOTOR;
	
	//PID CONTROLLER
	float controlSignal = Kp_m*err_m + Kierr_m + Kd_m*derr_m; // should be between 0 and 20.6m/s (3900*8.4*0.4*0.24*2*pi/60*62.5*10-3)
	
	int pwmSignal = (int)((controlSignal*500.0f)/20.6f)+1500;
	if(pwmSignal > 2000) pwmSignal = 2000;
	if(pwmSignal < 1500) pwmSignal = 1500;

	return pwmSignal; 
}

void read_Imu(sensor_msgs::Imu imu_msg)
{
	//save the time of the aquisition
	previousTime = currentTime;
	currentTime = imu_msg.header.stamp;

	//current roll angle
	currentRoll = imu_msg.orientation.x;
	ROS_INFO("Time %d", the_time);

	//keep calibration after 15 seconds
	if(the_time < 15) RollOffset = currentRoll;

	currentRoll -= RollOffset;
	ROS_INFO("New Roll %f", currentRoll);
}

int main(int argc, char **argv)
{

	int saturation = 2000;
	int freq = 100;
	Kp_m = 0;
	Ki_m = 0;
	Kd_m = 0;

	ROS_INFO("number of argc %d", argc);

	if(argc == 1)
	{
		//case with default params
	}
	else if(argc == 2)
	{
		//case with frequency
		if(atoi(argv[1]) > 0 )
			freq = atoi(argv[1]);
		else
		{
			ROS_INFO("Frequency must be more than 0");
			return 0;
		}
	}
	else if(argc == 3)
	{
		//case with frequency and saturation
		if(atoi(argv[1]) > 0 )
			freq = atoi(argv[1]);
		else
		{
			ROS_INFO("Frequency must be more than 0");
			return 0;
		}
	
		if(atoi(argv[2]) > 2000) saturation = 2000;
		else saturation = atoi(argv[2]);
	}
	else if(argc == 6)
	{
		//case with frequency and saturation and PID for motor
		if(atoi(argv[1]) > 0 )
			freq = atoi(argv[1]);
		else
		{
			ROS_INFO("Frequency must be more than 0");
			return 0;
		}
	
		if(atoi(argv[2]) > 2000) saturation = 2000;
		else saturation = atoi(argv[2]);

		Kp_m = atof(argv[3]);
		Ki_m = atof(argv[4]);
		Kd_m = atof(argv[5]);
		
	}
	else
	{
		ROS_INFO("not enough arguments ! Specify throttle saturation.");
		return 0;
	}

	ROS_INFO("frequency %d, and saturation  : %d", freq, saturation);


 	/***********************/
	/* Initialize The Node */
	/***********************/
	ros::init(argc, argv, "remote_reading_handler");
	ros::NodeHandle n;
	ros::Publisher remote_pub = n.advertise<sensor_msgs::Temperature>("remote_readings", 1000);
	ros::Publisher control_pub = n.advertise<sensor_msgs::Temperature>("control_readings", 1000);
	
	//subscribe to imu topic
	ros::Subscriber imu_sub = n.subscribe("imu_readings", 1000, read_Imu);

	//running rate = freq Hz
	ros::Rate loop_rate(freq);

	/****************************/
	/* Initialize the PID Stuff */
	/****************************/
	
	//Roll Control
	currentRoll = 0;
	currentTime = ros::Time::now();
	previousTime = ros::Time::now();
	Kierr1 = 0;
	err1 = 0;
	derr1 = 0;
	Kierr2 = 0;
	err2 = 0;
	derr2 = 0;

	//Motor Control
	currentSpeed = 0;
	currentTimeSpeed = ros::Time::now();
	previousTimeSpeed = ros::Time::now();
	Kierr_m = 0;
	err_m = 0;
	derr_m = 0;
	
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

	motor.set_period(MOTOR_PWM_OUT, 50); //frequency 50Hz for PWM
	servo.set_period(SERVO_PWM_OUT, 50); 

	int motor_input = 0;
	int servo_input = 0;

	sensor_msgs::Temperature rem_msg; //use of Temperature type messages. Because 2 floats
	sensor_msgs::Temperature ctrl_msg;

	float desired_roll = 0;
	float desired_speed = 0;

	//speed in m/s
	float speed = 0;
	float speed_filt = 0;
	int dtf = 0;// dtf read from arduino. dtf = dt*4 in msec
	float R = 0.0625f; //Rear Wheel Radius

	RollOffset = 0;
	int initTime = ros::Time::now().sec%1000;

	/*******************************************/
	/*             MAIN ROS LOOP               */
	/*******************************************/

	while (ros::ok())
	{

		/*******************************************/
		/*             ROLL SECTION                */
		/*******************************************/

		//read desired roll angle with remote ( 1250 to 1750 ) to range limited by defines
		desired_roll = -((float)rcin.read(2)-1500.0f)*max_roll_angle/250.0f;

		/*******************************************/
		/*             VELOCITY SECTION            */
		/*******************************************/

		//Get Desired PWM Speed using Throttle saturation
		int desired_pwm = 0;
		desired_pwm = ((float)rcin.read(3)-1500.0f)*((float)saturation - 1500.0f)/500.0f + 1500.0f;		
		//if(rcin.read(3) >= saturation)
		//	desired_pwm = saturation;
		//else
		//	desired_pwm = rcin.read(3);

		//get derired speed in m/s using desired pwm
		desired_speed = 20.6f*((float)desired_pwm-1500)/(500.0f);
		if(desired_speed < 0) desired_speed = 0.0f;

		//Read current Speed in m/s
		dtf = rcin.read(5)-1000;
		speed = 4.0f*PI*R*1000.0f/((float)dtf);
		if(speed < 0 || dtf < 40) speed = 0;
		
		// low pass filtering of the speed with tau = 0.1
		float alpha = (1.0f/freq)/((1.0f/freq)+0.1f);
		speed_filt = alpha*speed + (1.0f-alpha)*speed_filt;

		//update time for speed control
		currentSpeed = speed_filt;
		previousTimeSpeed = currentTimeSpeed;
		currentTimeSpeed = ros::Time::now();

		//calculate output to motor from pid controller
		motor_input = pid_Motor_Output(desired_speed);
		if(desired_pwm < 1500)
			motor_input = desired_pwm;

		//calculate output to servo from pid controller
		servo_input = pid_Servo_Output(pid_Ref_Output(desired_roll));
		
		//write readings on pwm output
		motor.set_duty_cycle(MOTOR_PWM_OUT, ((float)motor_input)/1000.0f); 
		servo.set_duty_cycle(SERVO_PWM_OUT, ((float)servo_input)/1000.0f);

		//Measure time for initial roll calibration
		the_time = ros::Time::now().sec%1000-initTime;
	
		/*******************************************/
		/*            MESSAGING SECTION            */
		/*******************************************/

		//save values into msg container
		rem_msg.header.stamp = ros::Time::now();
		rem_msg.temperature = desired_speed;
		rem_msg.variance = desired_roll;

		//save values into msg container for the control readings
		ctrl_msg.header.stamp = ros::Time::now();
		ctrl_msg.temperature = currentSpeed;
		ctrl_msg.variance = currentRoll;

		//publish messages
		remote_pub.publish(rem_msg);
		control_pub.publish(ctrl_msg);

		/*******************************************/
		/*            LOOPING SECTION              */
		/*******************************************/

		ros::spinOnce();

		loop_rate.sleep();

	}


  	return 0;
}


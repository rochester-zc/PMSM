/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Pavlo Milo Manovi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file	BasicMotorControl.c
 * @author 	Pavlo Manovi
 * @date	March 21st, 2014
 * @brief 	This library provides implementation of control methods for the DRV8301.
 *
 * This library provides implementation of a 3rd order LQG controller for a PMSM motor with
 * block commutation provided by hall effect sensors and a Change Notification interrupt system.
 * This LQG controller has the estimator and all other values exposed for experimentation.
 *
 * As of Sept. 2014 this method is included only as a method of integrating BLDC motors easily 
 * with the PMSMx module.  For use with the SUPER-Ball Bot and the geared Maxon Motors, please
 * use PMSM.c for motor control with the #SINE definition.
 * 
 * Note: To use this method of motor control CN must be set up.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "BasicMotorControl.h"
#include "PMSMBoard.h"
#include "DMA_Transfer.h"
#include "PMSM.h"
#include <uart.h>
#include <xc.h>
#include <dsp.h>
#include <math.h>
#include <qei32.h>


#ifndef CHARACTERIZE
#ifndef LQG_NOISE
#ifndef SINE

/**
 * @brief Converts control input into hall scaling.
 * @param speed radians per second to convert.
 * 
 * Takes a control input and scales it into hall counts.
 */
float Counts2RadSec(int16_t speed);


/**
 * @brief Linear Quadradic State Estimation
 *
 * All the state esimates in the Gaussian Estimator are visible here.
 */
static float u = 0;
static float Ts = .0003333;

static float x_hat[3][1] = {
	{0},
	{0},
	{0},
};

static float x_dummy[3][1] = {
	{0},
	{0},
	{0},
};

static float K_reg[3][3] = {
	{0.7639, -0.358, -0.5243},
	{0.2752, -0.1471, -0.55},
	{-0.2592, 0.4365, 0.6546},
};

static float L[3][1] = {
	{.0002849},
	{-.00008373},
	{-.001217},
};

static float K[1][3] = {
	{-0.4137, -0.6805, 0.744}
};

/**
 * @brief Called by CN Interrupt to update commutation.
 */
void TrapUpdate(uint16_t torque, uint16_t direction);


/**                          Public Functions                                **/


/**
 * @brief Call this to update the controller at a rate of 3kHz.
 * @param speed Speed to set controller to follow.
 * 
 * It is required that the LQG controller which was characterized at a sample rate of n Hz is
 * run every n Hz with this function call.
 */
void SpeedControlStep(float speed)
{
	uint16_t size = 0;
	static uint8_t out[56];
	qeiCounter w;
	w.l = 0;
	int16_t indexCount = 0;

	indexCount = (int) Read32bitQEI1IndexCounter();
	Write32bitQEI1IndexCounter(&w);

	float theta_dot = Counts2RadSec(indexCount) - speed;

	x_dummy[0][0] = (x_hat[0][0] * K_reg[0][0]) + (x_hat[1][0] * K_reg[0][1]) + (x_hat[2][0] * K_reg[0][2]) + (L[0][0] * theta_dot);
	x_dummy[1][0] = (x_hat[1][0] * K_reg[1][0]) + (x_hat[1][0] * K_reg[1][1]) + (x_hat[2][0] * K_reg[1][2]) + (L[1][0] * theta_dot);
	x_dummy[2][0] = (x_hat[2][0] * K_reg[2][0]) + (x_hat[1][0] * K_reg[2][1]) + (x_hat[2][0] * K_reg[2][2]) + (L[2][0] * theta_dot);

	x_hat[0][0] = x_dummy[0][0];
	x_hat[1][0] = x_dummy[1][0];
	x_hat[2][0] = x_dummy[2][0];

	u = -1 * ((K[0][0] * x_hat[0][0]) + (K[0][1] * x_hat[1][0]) + (K[0][2] * x_hat[2][0]));

	if (u < 0) {
		TrapUpdate((uint16_t) (-1 * u * PTPER), CCW);
	} else if (u >= 0) {
		TrapUpdate((uint16_t) (u * PTPER), CW);
	}

	size = sprintf((char *) out, "%i,%u\r\n", indexCount, (uint16_t) (x_hat[2][0] * 10000));
	DMA0_UART2_Transfer(size, out);

	LED4 ^= 1;
}

float Counts2RadSec(int16_t speed)
{
	return(((speed / (512.0)) * 2 * PI) / Ts);
}

/**
 * This function should exclusively be called by the change notify interrupt to
 * ensure that all hall events have been recorded and that the time between events
 * is appropriately read.
 *
 * The only other conceivable time that this would be called is if the motor is currently
 * not spinning.
 *
 * TODO: Look into preempting this interrupt and turning off interrupts when adding
 * data to the circular buffers as they are non-reentrant.  Calling this function
 * may break those libraries.
 * @param torque
 * @param direction
 */
void TrapUpdate(uint16_t torque, uint16_t direction)
{
	if (torque > PTPER) {
		torque = PTPER;
	}

	if (direction == CW) {
		if ((HALL1 && HALL2 && !HALL3)) {
			GH_A_DC = 0;
			GL_A_DC = 0;
			GH_B_DC = torque;
			GL_B_DC = 0;
			GH_C_DC = 0;
			GL_C_DC = torque;
			LED1 = 1;
			LED2 = 1;
			LED3 = 0;
		} else if ((!HALL1 && HALL2 && !HALL3)) {
			GH_A_DC = 0;
			GL_A_DC = torque;
			GH_B_DC = torque;
			GL_B_DC = 0;
			GH_C_DC = 0;
			GL_C_DC = 0;
			LED1 = 0;
			LED2 = 1;
			LED3 = 0;
		} else if ((!HALL1 && HALL2 && HALL3)) {
			GH_A_DC = 0;
			GL_A_DC = torque;
			GH_B_DC = 0;
			GL_B_DC = 0;
			GH_C_DC = torque;
			GL_C_DC = 0;
			LED1 = 0;
			LED2 = 1;
			LED3 = 1;
		} else if ((!HALL1 && !HALL2 && HALL3)) {
			GH_A_DC = 0;
			GL_A_DC = 0;
			GH_B_DC = 0;
			GL_B_DC = torque;
			GH_C_DC = torque;
			GL_C_DC = 0;
			LED1 = 0;
			LED2 = 0;
			LED3 = 1;
		} else if ((HALL1 && !HALL2 && HALL3)) {
			GH_A_DC = torque;
			GL_A_DC = 0;
			GH_B_DC = 0;
			GL_B_DC = torque;
			GH_C_DC = 0;
			GL_C_DC = 0;
			LED1 = 1;
			LED2 = 0;
			LED3 = 1;
		} else if ((HALL1 && !HALL2 && !HALL3)) {
			GH_A_DC = torque;
			GL_A_DC = 0;
			GH_B_DC = 0;
			GL_B_DC = 0;
			GH_C_DC = 0;
			GL_C_DC = torque;
			LED1 = 1;
			LED2 = 0;
			LED3 = 0;
		}
	} else if (direction == CCW) {
		if ((HALL1 && HALL2 && !HALL3)) {
			GH_A_DC = 0;
			GL_A_DC = 0;
			GH_B_DC = 0;
			GL_B_DC = torque;
			GH_C_DC = torque;
			GL_C_DC = 0;
			LED1 = 1;
			LED2 = 1;
			LED3 = 0;
		} else if ((!HALL1 && HALL2 && !HALL3)) {
			GH_A_DC = torque;
			GL_A_DC = 0;
			GH_B_DC = 0;
			GL_B_DC = torque;
			GH_C_DC = 0;
			GL_C_DC = 0;
			LED1 = 0;
			LED2 = 1;
			LED3 = 0;
		} else if ((!HALL1 && HALL2 && HALL3)) {
			GH_A_DC = torque;
			GL_A_DC = 0;
			GH_B_DC = 0;
			GL_B_DC = 0;
			GH_C_DC = 0;
			GL_C_DC = torque;
			LED1 = 0;
			LED2 = 1;
			LED3 = 1;
		} else if ((!HALL1 && !HALL2 && HALL3)) {
			GH_A_DC = 0;
			GL_A_DC = 0;
			GH_B_DC = torque;
			GL_B_DC = 0;
			GH_C_DC = 0;
			GL_C_DC = torque;
			LED1 = 0;
			LED2 = 0;
			LED3 = 1;
		} else if ((HALL1 && !HALL2 && HALL3)) {
			GH_A_DC = 0;
			GL_A_DC = torque;
			GH_B_DC = torque;
			GL_B_DC = 0;
			GH_C_DC = 0;
			GL_C_DC = 0;
			LED1 = 1;
			LED2 = 0;
			LED3 = 1;
		} else if ((HALL1 && !HALL2 && !HALL3)) {

			GH_A_DC = 0;
			GL_A_DC = torque;
			GH_B_DC = 0;
			GL_B_DC = 0;
			GH_C_DC = torque;
			GL_C_DC = 0;
			LED1 = 1;
			LED2 = 0;
			LED3 = 0;
		}
	}
}

void __attribute__((__interrupt__, no_auto_psv)) _CNInterrupt(void)
{
	if (u < 0) {
		TrapUpdate((uint16_t) (-1 * u * PTPER), CCW);
	} else if (u >= 0) {
		TrapUpdate((uint16_t) (u * PTPER), CW);
	}
	IFS1bits.CNIF = 0; // Clear CN interrupt
}

void __attribute__((__interrupt__, no_auto_psv)) _QEI1Interrupt(void)
{
	IFS3bits.QEI1IF = 0; /* Clear QEI interrupt flag */
}
#endif
#endif
#endif

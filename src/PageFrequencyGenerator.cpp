/*
 * PageFrequencyGenerator.cpp
 *
 * Frequency output from 119 mHz (8.388 second) to 8MHz on Arduino
 * Waveform frequency is not stable and decreased when DSO is running since then not all overflow interrupts can be handled
 *
 *  Copyright (C) 2015  Armin Joachimsmeyer
 *  Email: armin.joachimsmeyer@gmail.com
 *
 *  This file is part of Arduino-Simple-DSO https://github.com/ArminJo/Arduino-Simple-DSO.
 *
 *  Arduino-Simple-DSO is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/gpl.html>.
 *
 */

#ifdef AVR
#include "PageFrequencyGenerator.h"
#include "Waveforms.h"
#include "BlueDisplay.h"

#include "SimpleTouchScreenDSO.h"

#include <stdlib.h> // for dtostrf

#else
#include "Pages.h"
#include "main.h" // for StringBuffer
#include "TouchDSO.h"
#endif

#include <stdio.h>   // for printf
#include <math.h>   // for pow and log10f

static void (*sLastRedrawCallback)(void);

#define COLOR_BACKGROUND_FREQ COLOR_WHITE

#ifdef AVR
#define TIMER_PRESCALER_64 0x03
#define TIMER_PRESCALER_MASK 0x07
#endif

#define NUMBER_OF_FIXED_FREQUENCY_BUTTONS 10
#define NUMBER_OF_FREQUENCY_RANGE_BUTTONS 5

/*
 * Position + size
 */
#define FREQ_SLIDER_SIZE 10 // width of bar / border
#define FREQ_SLIDER_MAX_VALUE 300 // (BlueDisplay1.getDisplayWidth() - 20) = 300 length of bar
#define FREQ_SLIDER_X 5
#define FREQ_SLIDER_Y (4 * TEXT_SIZE_11_HEIGHT + 4)

/*
 * Direct frequency + range buttons
 */
#ifdef AVR
const uint16_t FixedFrequencyButtonCaptions[NUMBER_OF_FIXED_FREQUENCY_BUTTONS] PROGMEM
= { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000 };

// the compiler cannot optimize 2 occurrences of the same PROGMEM string
const char StringmHz[] PROGMEM = "mHz";
const char StringHz[] PROGMEM = "Hz";
const char String10Hz[] PROGMEM = "10Hz";
const char StringkHz[] PROGMEM = "kHz";
const char StringMHz[] PROGMEM = "MHz";

const char* RangeButtonStrings[5] = { StringmHz, StringHz, String10Hz, StringkHz, StringMHz };
#else
const uint16_t FixedFrequencyButtonCaptions[NUMBER_OF_FIXED_FREQUENCY_BUTTONS] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
const char* const RangeButtonStrings[5] = {"mHz", "Hz", "10Hz", "kHz", "MHz"};
const char FrequencyFactorChars[4] = { 'm', ' ', 'k', 'M' };
#endif

#define INDEX_OF_10HZ 2

#ifndef AVR

#define WAVEFORM_SQUARE 0
#define WAVEFORM_SINE 1
#define WAVEFORM_TRIANGLE 2
#define WAVEFORM_SAWTOOTH 3

struct FrequencyInfoStruct {
    union {
        uint32_t DividerInt; // Value used by hardware - may be (divider * prescaler)
        uint32_t sBaseFrequencyFactorShift16;// Value use by ISR
    }ControlValue;
    uint32_t PeriodMicros; // for CTC resolution of value of DividerInt is 8 times better
    // use float, since we have a logarithmic slider readout and therefore a lot of values between 1 and 2.
    float Frequency;// Computed value derived from sPeriodInt
    // factor for mHz/Hz/kHz/MHz - times 1000 because of mHz handling
    uint32_t FrequencyFactorTimes1000;// 1 -> 1 mHz, 1000 -> 1 Hz, 1000000 -> 1 kHz
    uint8_t FrequencyFactorIndex;// 0->mHz, 1->Hz, 2->kHz, 3->MHz
    uint8_t Waveform;
    bool isOutputEnabled;
};
struct FrequencyInfoStruct sFrequencyInfo;

void setFrequencyFactor(int aIndexValue) {
    sFrequencyInfo.FrequencyFactorIndex = aIndexValue;
    uint32_t tFactor = 1;
    while (aIndexValue > 0) {
        tFactor *= 1000;
        aIndexValue--;
    }
    sFrequencyInfo.FrequencyFactorTimes1000 = tFactor;
}

const char * getWaveformModePGMString() {
    const char * tResultString;
    tResultString = PSTR("Square");
    if (sFrequencyInfo.Waveform == WAVEFORM_SINE) {
        tResultString = PSTR("Sine");
    } else if (sFrequencyInfo.Waveform == WAVEFORM_TRIANGLE) {
        tResultString = PSTR("Triangle");
    } else if (sFrequencyInfo.Waveform == WAVEFORM_SAWTOOTH) {
        tResultString = PSTR("Sawtooth");
    }
    return tResultString;
}

bool setWaveformFrequency() {
    bool hasError = false;
    if (sFrequencyInfo.Waveform == WAVEFORM_SQUARE) {
        float tPeriod = (36000000000 / sFrequencyInfo.FrequencyFactorTimes1000) / sFrequencyInfo.Frequency;
        uint32_t tPeriodInt = tPeriod;
        if (tPeriodInt < 2) {
            hasError = true;
            tPeriodInt = 2;
        }

#ifdef STM32F30X
        Synth_Timer32_SetReloadValue(tPeriodInt);
#else
        uint32_t tPrescalerValue = (tPeriodInt >> 16) + 1; // +1 since at least divider by 1
        if (tPrescalerValue > 1) {
            //we have prescaler > 1 -> adjust reload value to be less than 0x10001
            tPeriodInt /= tPrescalerValue;
        }
        Synth_Timer16_SetReloadValue(tPeriodInt, tPrescalerValue);
        tPeriodInt *= tPrescalerValue;
#endif
        sFrequencyInfo.ControlValue.DividerInt = tPeriodInt;
    } else {
        hasError = true;
    }
    return hasError;
}

void setFrequency(float aValue) {
    if (sFrequencyInfo.Waveform == WAVEFORM_SQUARE) {
        uint8_t tIndex = 1;
        while (aValue > 1000) {
            aValue /= 1000;
            tIndex++;
        }
        if (aValue < 1) {
            tIndex = 0; //mHz
            aValue *= 1000;
        }
        setFrequencyFactor(tIndex);
    }
    sFrequencyInfo.Frequency = aValue;
    setWaveformFrequency();
}
#endif

static const int BUTTON_INDEX_SELECTED_INITIAL = 2; // select 10Hz Button
static bool is10HzRange = true;

/*
 * GUI
 */
BDButton TouchButtonFrequencyRanges[NUMBER_OF_FREQUENCY_RANGE_BUTTONS];
BDButton ActiveTouchButtonFrequencyRange; // Used to determine which range button is active

BDButton TouchButtonFrequencyStartStop;
BDButton TouchButtonGetFrequency;
BDButton TouchButtonWaveform;

#ifdef LOCAL_DISPLAY_EXISTS
BDButton TouchButton1;
BDButton TouchButton2;
BDButton TouchButton5;
BDButton TouchButton10;
BDButton TouchButton20;
BDButton TouchButton50;
BDButton TouchButton100;
BDButton TouchButton200;
BDButton TouchButton500;
BDButton TouchButton1k;
BDButton * const TouchButtonFixedFrequency[] = {&TouchButton1, &TouchButton2, &TouchButton5, &TouchButton10, &TouchButton20,
    &TouchButton50, &TouchButton100, &TouchButton200, &TouchButton500, &TouchButton1k};
#else
BDButton TouchButtonFirstFixedFrequency;
#endif

BDSlider TouchSliderFrequency;

void initFrequencyGeneratorPageGui(void);

void doFrequencySlider(BDSlider * aTheTouchedSlider, uint16_t aValue);

void doWaveformMode(BDButton * aTheTouchedButton, int16_t aValue);
void setWaveformButtonCaption(void);
void doSetFixedFrequency(BDButton * aTheTouchedButton, int16_t aValue);
void doChangeFrequencyRange(BDButton * aTheTouchedButton, int16_t aValue);
void doFrequencyGeneratorStartStop(BDButton * aTheTouchedButton, int16_t aValue);
void doGetFrequency(BDButton * aTheTouchedButton, int16_t aValue);
bool SetWaveformFrequencyAndPrintValues();
void printFrequencyAndPeriod();
#ifdef AVR
void initTimer1ForCTC(void);
#else
#endif

/***********************
 * Code starts here
 ***********************/

void initFrequencyGenerator(void) {
#ifdef AVR
    initTimer1ForCTC();
#else
    // set frequency to 2 kHz
    Synth_Timer_initialize(36000);
#endif
}

void initFrequencyGeneratorPage(void) {
    /*
     * Initialize frequency and other fields to 200 Hz
     */
    sFrequencyInfo.Waveform = WAVEFORM_SQUARE;
    setFrequency(200);
    sFrequencyInfo.isOutputEnabled = true; // to start output at first display of page

#ifndef LOCAL_DISPLAY_EXISTS
    initFrequencyGeneratorPageGui();
#endif
}

void startFrequencyGeneratorPage(void) {
    BlueDisplay1.clearDisplay(COLOR_BACKGROUND_FREQ);

#ifdef LOCAL_DISPLAY_EXISTS
    // do it here to enable freeing of button resources in stopFrequencyGeneratorPage()
    initFrequencyGeneratorPageGui();
#endif

    drawFrequencyGeneratorPage();
    setWaveformFrequency();
    /*
     * save state
     */
    sLastRedrawCallback = getRedrawCallback();
    registerRedrawCallback(&drawFrequencyGeneratorPage);

#ifndef AVR
    Synth_Timer_Start();
#endif
}

void loopFrequencyGeneratorPage(void) {
    checkAndHandleEvents();
}

void stopFrequencyGeneratorPage(void) {
#ifdef LOCAL_DISPLAY_EXISTS
    // free buttons
    for (unsigned int i = 0; i < NUMBER_OF_FIXED_FREQUENCY_BUTTONS; ++i) {
        TouchButtonFixedFrequency[i]->deinit();
    }

    for (int i = 0; i < NUMBER_OF_FREQUENCY_RANGE_BUTTONS; ++i) {
        TouchButtonFrequencyRanges[i].deinit();
    }
    TouchButtonFrequencyStartStop.deinit();
    TouchButtonGetFrequency.deinit();
    TouchSliderFrequency.deinit();
    TouchButtonWaveform.deinit();
#endif
    /*
     * restore previous state
     */
    registerRedrawCallback(sLastRedrawCallback);
}

void initFrequencyGeneratorPageGui() {
    // Frequency slider for 1 to 1000 at top of screen
    TouchSliderFrequency.init(FREQ_SLIDER_X, FREQ_SLIDER_Y, FREQ_SLIDER_SIZE, FREQ_SLIDER_MAX_VALUE,
    FREQ_SLIDER_MAX_VALUE, 0, COLOR_BLUE, COLOR_GREEN, FLAG_SLIDER_SHOW_BORDER | FLAG_SLIDER_IS_HORIZONTAL, &doFrequencySlider);

    /*
     * Fixed frequency buttons next. Example of button handling without button objects
     */
    uint16_t tXPos = 0;
    uint16_t tFrequency;
#ifdef AVR
    // captions are in PGMSPACE
    const uint16_t * tFrequencyCaptionPtr = &FixedFrequencyButtonCaptions[0];
    for (uint8_t i = 0; i < NUMBER_OF_FIXED_FREQUENCY_BUTTONS; ++i) {
        tFrequency = pgm_read_word(tFrequencyCaptionPtr);
        sprintf_P(sStringBuffer, PSTR("%u"), tFrequency);
#else
        for (uint8_t i = 0; i < NUMBER_OF_FIXED_FREQUENCY_BUTTONS; ++i) {
            tFrequency = FixedFrequencyButtonCaptions[i];
            sprintf(sStringBuffer, "%u", tFrequency);
#endif

#ifdef LOCAL_DISPLAY_EXISTS
        TouchButtonFixedFrequency[i]->init(tXPos,
                REMOTE_DISPLAY_HEIGHT - BUTTON_HEIGHT_4 - BUTTON_HEIGHT_5 - BUTTON_HEIGHT_6 - 2 * BUTTON_DEFAULT_SPACING,
                BUTTON_WIDTH_10, BUTTON_HEIGHT_6, COLOR_BLUE, sStringBuffer, TEXT_SIZE_11, 0, tFrequency, &doSetFixedFrequency);
#else
        TouchButtonFirstFixedFrequency.init(tXPos,
                REMOTE_DISPLAY_HEIGHT - BUTTON_HEIGHT_4 - BUTTON_HEIGHT_5 - BUTTON_HEIGHT_6 - 2 * BUTTON_DEFAULT_SPACING,
                BUTTON_WIDTH_10, BUTTON_HEIGHT_6, COLOR_BLUE, sStringBuffer, TEXT_SIZE_11, 0, tFrequency, &doSetFixedFrequency);
#endif

        tXPos += BUTTON_WIDTH_10 + BUTTON_DEFAULT_SPACING_QUARTER;
#ifdef AVR
        tFrequencyCaptionPtr++;
#endif
    }
#ifndef LOCAL_DISPLAY_EXISTS
    TouchButtonFirstFixedFrequency.mButtonHandle -= NUMBER_OF_FIXED_FREQUENCY_BUTTONS - 1;
#endif

    // Range next
    tXPos = 0;
    int tYPos = REMOTE_DISPLAY_HEIGHT - BUTTON_HEIGHT_4 - BUTTON_HEIGHT_5 - BUTTON_DEFAULT_SPACING;
    for (int i = 0; i < NUMBER_OF_FREQUENCY_RANGE_BUTTONS; ++i) {
        uint16_t tButtonColor = BUTTON_AUTO_RED_GREEN_FALSE_COLOR;
        if (i == BUTTON_INDEX_SELECTED_INITIAL) {
            tButtonColor = BUTTON_AUTO_RED_GREEN_TRUE_COLOR;
        }
        TouchButtonFrequencyRanges[i].initPGM(tXPos, tYPos, BUTTON_WIDTH_5 + BUTTON_DEFAULT_SPACING_HALF,
        BUTTON_HEIGHT_5, tButtonColor, RangeButtonStrings[i], TEXT_SIZE_22, FLAG_BUTTON_DO_BEEP_ON_TOUCH, i,
                &doChangeFrequencyRange);

        tXPos += BUTTON_WIDTH_5 + BUTTON_DEFAULT_SPACING - 2;
    }

    ActiveTouchButtonFrequencyRange = TouchButtonFrequencyRanges[BUTTON_INDEX_SELECTED_INITIAL];

    TouchButtonFrequencyStartStop.initPGM(0, REMOTE_DISPLAY_HEIGHT - BUTTON_HEIGHT_4, BUTTON_WIDTH_3, BUTTON_HEIGHT_4, 0,
            PSTR("Start"), TEXT_SIZE_26, FLAG_BUTTON_DO_BEEP_ON_TOUCH | FLAG_BUTTON_TYPE_TOGGLE_RED_GREEN,
            sFrequencyInfo.isOutputEnabled, &doFrequencyGeneratorStartStop);
    TouchButtonFrequencyStartStop.setCaptionPGMForValueTrue(PSTR("Stop"));

    TouchButtonGetFrequency.initPGM(BUTTON_WIDTH_3_POS_2, REMOTE_DISPLAY_HEIGHT - BUTTON_HEIGHT_4, BUTTON_WIDTH_3,
    BUTTON_HEIGHT_4, COLOR_BLUE, PSTR("Hz..."), TEXT_SIZE_22, FLAG_BUTTON_DO_BEEP_ON_TOUCH, 0, &doGetFrequency);

    TouchButtonWaveform.init(BUTTON_WIDTH_3_POS_3, REMOTE_DISPLAY_HEIGHT - BUTTON_HEIGHT_4, BUTTON_WIDTH_3,
    BUTTON_HEIGHT_4, COLOR_BLUE, "", TEXT_SIZE_18, FLAG_BUTTON_DO_BEEP_ON_TOUCH, sFrequencyInfo.Waveform, &doWaveformMode);
    setWaveformButtonCaption();
}

void drawFrequencyGeneratorPage(void) {
    // do not clear screen here since it is called periodically for GUI refresh while DSO is running
    BDButton::deactivateAllButtons();
    BDSlider::deactivateAllSliders();
#ifdef LOCAL_DISPLAY_EXISTS
    TouchButtonMainHome.drawButton();
#else
    TouchButtonBack.drawButton();
#endif
    TouchSliderFrequency.drawSlider();

#ifdef AVR
    BlueDisplay1.drawTextPGM(TEXT_SIZE_11_WIDTH, FREQ_SLIDER_Y + 3 * FREQ_SLIDER_SIZE + TEXT_SIZE_11_HEIGHT, PSTR("1"),
    TEXT_SIZE_11, COLOR_BLUE, COLOR_BACKGROUND_FREQ);
    BlueDisplay1.drawTextPGM(REMOTE_DISPLAY_WIDTH - 5 * TEXT_SIZE_11_WIDTH,
    FREQ_SLIDER_Y + 3 * FREQ_SLIDER_SIZE + TEXT_SIZE_11_HEIGHT, PSTR("1000"), TEXT_SIZE_11, COLOR_BLUE,
    COLOR_BACKGROUND_FREQ);
#else
    BlueDisplay1.drawText(TEXT_SIZE_11_WIDTH, FREQ_SLIDER_Y + 3 * FREQ_SLIDER_SIZE + TEXT_SIZE_11_HEIGHT, ("1"),
            TEXT_SIZE_11, COLOR_BLUE, COLOR_BACKGROUND_FREQ);
    BlueDisplay1.drawText(BlueDisplay1.getDisplayWidth() - 5 * TEXT_SIZE_11_WIDTH,
            FREQ_SLIDER_Y + 3 * FREQ_SLIDER_SIZE + TEXT_SIZE_11_HEIGHT, ("1000"), TEXT_SIZE_11, COLOR_BLUE,
            COLOR_BACKGROUND_FREQ);
#endif

    // fixed frequency buttons
    // we know that the buttons handles are increasing numbers
#ifndef LOCAL_DISPLAY_EXISTS
    BDButton tButton(TouchButtonFirstFixedFrequency);
#endif
#ifdef LOCAL_DISPLAY_EXISTS
    for (uint8_t i = 0; i < NUMBER_OF_FIXED_FREQUENCY_BUTTONS - 1; ++i) {
        // Generate strings each time buttons are drawn since only the pointer to caption is stored in button
        sprintf(sStringBuffer, "%u", FixedFrequencyButtonCaptions[i]);
        TouchButtonFixedFrequency[i]->setCaption(sStringBuffer);
        TouchButtonFixedFrequency[i]->drawButton();
    }
    // label last button 1k instead of 1000 which is too long
    TouchButtonFixedFrequency[NUMBER_OF_FIXED_FREQUENCY_BUTTONS - 1]->setCaption("1k");
    TouchButtonFixedFrequency[NUMBER_OF_FIXED_FREQUENCY_BUTTONS - 1]->drawButton();
#else
    for (uint8_t i = 0; i < NUMBER_OF_FIXED_FREQUENCY_BUTTONS; ++i) {
        tButton.drawButton();
        tButton.mButtonHandle++;
    }
#endif

    for (uint8_t i = 0; i < NUMBER_OF_FREQUENCY_RANGE_BUTTONS; ++i) {
        TouchButtonFrequencyRanges[i].drawButton();
    }

    TouchButtonFrequencyStartStop.drawButton();
    TouchButtonGetFrequency.drawButton();
    TouchButtonWaveform.drawButton();

    // show values
    printFrequencyAndPeriod();
}

/*
 * Slider handlers
 */
void doFrequencySlider(BDSlider * aTheTouchedSlider, uint16_t aValue) {
    float tValue = aValue;
    tValue = tValue / (FREQ_SLIDER_MAX_VALUE / 3); // gives 0-3
    if (is10HzRange) {
        tValue += 1;
    }
    // 950 byte program space needed for pow() and log10f()
    sFrequencyInfo.Frequency = pow(10, tValue);
    SetWaveformFrequencyAndPrintValues();
}

/*
 * Button handlers
 */
void setWaveformButtonCaption(void) {
    TouchButtonWaveform.setCaptionPGM(getWaveformModePGMString(), (DisplayControl.DisplayPage == DISPLAY_PAGE_FREQUENCY));
}

void doWaveformMode(BDButton * aTheTouchedButton, int16_t aValue) {
#ifdef AVR
    cycleWaveformMode();
    setWaveformButtonCaption();
#endif
}

/**
 * Set frequency to fixed value 1,2,5,10...,1000
 */
void doSetFixedFrequency(BDButton * aTheTouchedButton, int16_t aValue) {
    if (is10HzRange) {
        aValue *= 10;
    }
    sFrequencyInfo.Frequency = aValue;
#ifdef LOCAL_DISPLAY_EXISTS
    FeedbackTone(SetWaveformFrequencyAndPrintValues());
#else
    BlueDisplay1.playFeedbackTone(SetWaveformFrequencyAndPrintValues());
#endif
}

/**
 * changes the unit (mHz - MHz)
 * set color for old and new button
 */
void doChangeFrequencyRange(BDButton * aTheTouchedButton, int16_t aValue) {
    if (ActiveTouchButtonFrequencyRange != *aTheTouchedButton) {
        ActiveTouchButtonFrequencyRange.setButtonColorAndDraw( BUTTON_AUTO_RED_GREEN_FALSE_COLOR);
        ActiveTouchButtonFrequencyRange = *aTheTouchedButton;
        aTheTouchedButton->setButtonColorAndDraw( BUTTON_AUTO_RED_GREEN_TRUE_COLOR);
        // Handling of 10 Hz button
        if (aValue == INDEX_OF_10HZ) {
            is10HzRange = true;
        } else {
            is10HzRange = false;
        }
        if (aValue >= INDEX_OF_10HZ) {
            aValue--;
        }
        setFrequencyFactor(aValue);
        SetWaveformFrequencyAndPrintValues();
    }
}

#ifdef LOCAL_DISPLAY_EXISTS
/**
 * gets frequency value from numberpad
 * @param aTheTouchedButton
 * @param aValue
 */
void doGetFrequency(BDButton * aTheTouchedButton, int16_t aValue) {
    TouchSliderFrequency.deactivate();
    float tNumber = getNumberFromNumberPad(NUMBERPAD_DEFAULT_X, 0, COLOR_BLUE);
// check for cancel
    if (!isnan(tNumber)) {
        sFrequencyInfo.Frequency = tNumber;
    }
    drawFrequencyGeneratorPage();
    SetWaveformFrequencyAndPrintValues();
}
#else

/**
 * Handler for number receive event - set frequency to value
 */
void doSetFrequency(float aValue) {
    setFrequency(aValue);
    printFrequencyAndPeriod();
}

/**
 * Request frequency numerical
 */
void doGetFrequency(BDButton * aTheTouchedButton, int16_t aValue) {
    BlueDisplay1.getNumberWithShortPromptPGM(&doSetFrequency, PSTR("frequency [Hz]"));
}
#endif

void doFrequencyGeneratorStartStop(BDButton * aTheTouchedButton, int16_t aValue) {
    sFrequencyInfo.isOutputEnabled = aValue;
    if (aValue) {
        // Start timer
#ifndef AVR
        Synth_Timer_Start();
#endif
        SetWaveformFrequencyAndPrintValues();
    } else {
        // Stop timer
#ifdef AVR
        stopWaveform();
#else
        Synth_Timer_Stop();
#endif
    }
}

/*
 * uses global value Frequency and sPeriodInt
 */
void printFrequencyAndPeriod() {
    float tPeriodMicros;

#ifdef AVR
    dtostrf(sFrequencyInfo.Frequency, 9, 3, &sStringBuffer[20]);
    sprintf_P(sStringBuffer, PSTR("%s%cHz"), &sStringBuffer[20], FrequencyFactorChars[sFrequencyInfo.FrequencyFactorIndex]);
    if (sFrequencyInfo.Waveform == WAVEFORM_SQUARE) {
        tPeriodMicros = sFrequencyInfo.ControlValue.DividerInt;
        tPeriodMicros /= 8;
    } else {
        tPeriodMicros = sFrequencyInfo.PeriodMicros;
    }
#else
// recompute exact frequency for given integer period
    float tPeriodFloat = sFrequencyInfo.ControlValue.DividerInt;
    float tFrequency = (36000000000 / sFrequencyInfo.FrequencyFactorTimes1000) / tPeriodFloat;
    snprintf(sStringBuffer, sizeof sStringBuffer, "%9.3f%cHz", tFrequency,
            FrequencyFactorChars[sFrequencyInfo.FrequencyFactorIndex]);
    tPeriodMicros = tPeriodFloat;
    tPeriodMicros /= 36;
#endif
// print frequency
    BlueDisplay1.drawText(FREQ_SLIDER_X + 2 * TEXT_SIZE_22_WIDTH, TEXT_SIZE_22_HEIGHT, sStringBuffer, TEXT_SIZE_22,
    COLOR_RED, COLOR_BACKGROUND_FREQ);

// output period
    char tUnitChar = '\xB5';    // micro
    if (tPeriodMicros > 10000) {
        tPeriodMicros /= 1000;
        tUnitChar = 'm';
    }
#ifdef AVR
    dtostrf(tPeriodMicros, 10, 3, &sStringBuffer[20]);
    sprintf_P(sStringBuffer, PSTR("%s%cs"), &sStringBuffer[20], tUnitChar);
#else
    snprintf(sStringBuffer, sizeof sStringBuffer, "%10.3f%cs", tPeriodMicros, tUnitChar);
#endif
    BlueDisplay1.drawText(FREQ_SLIDER_X, TEXT_SIZE_22_HEIGHT + 4 + TEXT_SIZE_22_ASCEND, sStringBuffer, TEXT_SIZE_22,
    COLOR_BLUE, COLOR_BACKGROUND_FREQ);

// 950 byte program space needed for pow() and log10f()
    uint16_t tSliderValue = log10f(sFrequencyInfo.Frequency) * 100;
    if (is10HzRange) {
        tSliderValue -= 100;
    }
    TouchSliderFrequency.setActualValueAndDrawBar(tSliderValue);
}

/**
 * Computes Autoreload value for synthesizer from 8,381 mHz (0xFFFFFFFF) to 18MHz (0x02) and prints frequency value
 * @param aSetSlider
 * @param global variable sFrequency
 * @return true if error / clipping happened
 */
bool SetWaveformFrequencyAndPrintValues() {
    bool hasError = setWaveformFrequency();
    printFrequencyAndPeriod();
    return hasError;
}

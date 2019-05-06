#include "raptor.h"
#include <Streaming.h> // http://arduiniana.org/libraries/streaming/

// #define RC_TEST
#include "guidance/drivers/servo/continuous_servo.h"
/* 
 * Arduino setup function, first function to be run.
 */
Raptor::Raptor()
{
    /* Buzzer and LEDs */
    pinMode(BZZ_DTA, OUTPUT);  // Set buzzer to output
    pinMode(LEDS_DTA, OUTPUT); // Set LEDs to output

    Serial.begin(115200);

    /* Solenoids, Servos, BMP, BNO */
    parafoil_sol = new Solenoid(9, A0, A2);
    cutdown_sol = new Solenoid(8, A1, A3);

    // startup_sequence();

    /* GPS */
    environment = new Environment();
    environment->init(true); // for testing pcb

    if (digitalRead(SET_BTN))
    {
        eeprom->write_state(flight_state, environment->bmp->baseline);
    }

    eeprom = new Prom();
    pinMode(SET_BTN, OUTPUT);
    if (!digitalRead(SET_BTN))
    {
        Serial << "Read EEPROM\n";
        eeprom->read_state(&flight_state, &environment->bmp->baseline);

        // print retrieved data
        Serial << "Saved flight state: " << flight_state;
        Serial << "\nSaved baseline: " << environment->bmp->baseline << "\n";
    }

    pilot = new Pilot();

    delay(10);
    Serial.print(F("TIME, TEMPERATURE, PRESSURE, ALTITUDE, LATITUDE, LONGITUDE, ANGLE, GPS_ALT, X, Y, Z, SWC, SWP, TURN, FLIGHT_STATE\n")); // data header
}

void Raptor::launch()
{
    if (environment->bmp->getAltitude() > GROUND_ALT)
    { // at 50ft (15.24 meters), transition to FS1 [ASCENT]
        flight_state = 1;
        eeprom->write_state(flight_state, environment->bmp->baseline);
    }

    // blink the LEDs and print data at a rate of 1Hz
    blink_led(1000);
    print_data();
}

void Raptor::ascent()
{
    if (environment->bmp->getAltitude() > CUTDOWN_ALT)
    { // at the cutdown altitude perform cutdown, deploy, and transition to FS2 [DESCENT]
        // CUTDOWN
        this->cutdown_sol->open();

        if (!this->cutdown_sol->read_switch())
        { // we want to make sure that we have cut down
            Serial << F("\n!!!! CUTDOWN ERROR DETECTED !!!!\n");
            this->cutdown_sol->open(); // try cutdown again, probably won't do much
        }

        // PARAFOIL DEPLOY
        while (environment->bmp->getAltitude() > CUTDOWN_ALT - 3.048)
        { // wait 3 meters to deploy the parafoil
            delay(1);
            print_data();
        }

        this->parafoil_sol->open();

        if (!this->parafoil_sol->read_switch())
        { // make sure the parafoil has deployed
            Serial << F("\n!!!! PARAFOIL DEPLOYMENT ERROR DETECTED !!!!\n");
            this->parafoil_sol->open(); // try deploying parafoil again, probably won't do much
        }

        delay(DEPLOY_DELAY); // wait for the parafoil to deploy/inflate before we begin guidance

        flight_state = 2;
        eeprom->write_state(flight_state, environment->bmp->baseline);
    }

    // blink the LEDs and print data at a rate of 5Hz
    blink_led(200);
    print_data();
}

void Raptor::descent()
{
    // if we have yet to wake the pilot, do so
    if (!didwake)
    {
        // first set up our coordinates
        Coordinate current, target;

        current.latitude = environment->gps->getGPS()->latitude;
        current.longitude = environment->gps->getGPS()->longitude;

        target.latitude = TARGET_LAT;
        target.longitude = TARGET_LONG;

        // then wake the pilot and give it the coordinates
        Serial << "Waking pilot\n";
        pilot->wake(current, target);
        didwake = true;
    }

    fly_time = *environment->time_elapsed;
    if (fly_time > FLY_DELAY)
    {                                        // don't want to constantly call fly
        pilot->fly(environment->gps->getGPS()->angle); // the pilot just needs our current angle to do his calculations
        fly_time = 0;
    }

    if (environment->bmp->getAltitude() < GROUND_ALT)
    { // at 50ft (15.24 meters), transition to FS3 [LANDED]
        if (environment->landing_check())
        { // make sure that we have landed by checking the altitude constantly
            pilot->sleep();
            flight_state = 3;
            Serial << "\n!!!! LANDED !!!!\n";
        }
    }

    // blink the LEDs and print data at 10Hz
    blink_led(100);
    print_data();
}

void Raptor::landed()
{
    // in the landed state, only toggle the LEDs and buzzer every 2 seconds, then print data
    analogWrite(BZZ_DTA, 200);
    blink_led(2000);
    analogWrite(BZZ_DTA, 0);

    delay(200);
    print_data();
}

/*
 * print_data updates sensor readings then prints all relevant data to the serial pins.
 */
void Raptor::print_data()
{
    environment->update();

    /* Let's spray the serial port with a hose of data */
    // gps stuff can remove
    Serial.print("\nTime: ");
    Serial.print(environment->gps->getGPS()->hour, DEC);
    Serial.print(':');
    Serial.print(environment->gps->getGPS()->minute, DEC);
    Serial.print(':');
    Serial.print(environment->gps->getGPS()->seconds, DEC);
    Serial.print('.');
    Serial.println(environment->gps->getGPS()->milliseconds);
    Serial.print("Date: ");
    Serial.print(environment->gps->getGPS()->day, DEC);
    Serial.print('/');
    Serial.print(environment->gps->getGPS()->month, DEC);
    Serial.print("/20");
    Serial.println(environment->gps->getGPS()->year, DEC);
    Serial.print("Fix: ");
    Serial.print((int)environment->gps->getGPS()->fix);
    Serial.print(" quality: ");
    Serial.println((int)environment->gps->getGPS()->fixquality);

    // time, temperature, pressure, altitude,
    Serial << *environment->time_elapsed << F(",") << environment->bmp->readTemperature() << F(",") << environment->bmp->readPressure()
           << F(",") << environment->bmp->getAltitude() << F(",");

    // // latitude, longitude, angle, (gps) altitude,
    Serial << _FLOAT(environment->gps->getGPS()->latitude, 7) << F(",") << _FLOAT(environment->gps->getGPS()->longitude, 7)
           << F(",") << _FLOAT(environment->gps->getGPS()->angle, 7) << F(",") << environment->gps->getGPS()->altitude << F(",");

    // // x orientation, y orientation, z orientation,
    Serial << _FLOAT(environment->bno->data.orientation.x, 4) << F(",") << _FLOAT(environment->bno->data.orientation.y, 4)
           << F(",") << _FLOAT(environment->bno->data.orientation.z, 4) << F(",");

    // // cutdown switch, parafoil switch, turn status, flight state
    Serial << this->cutdown_sol->read_switch() << F(",") << this->parafoil_sol->read_switch() << F(",")
           << pilot->get_turn() << F(",") << flight_state << "\n"; // write everything to SD card
}

/* 
 *  startup_sequence intitializes our solenoids, servos, and sensors.
 *   If in flight state 0 (launch), performs a sequence that indicates board power,
 *   solenoid power, servo power, and successful sensor initialization.
 */
void Raptor::startup_sequence(void)
{
    // indicate board power with a buzzer beep if in flight state 0
    if (flight_state == 0)
    {
        analogWrite(BZZ_DTA, 200); // turn on the buzzer for half a second
        delay(500);
        analogWrite(BZZ_DTA, 0);
    }

    // intialize solenoids, should hear them click and see the indicator LEDs turn on
    parafoil_sol->close();
    parafoil_sol->read_switch();

    cutdown_sol->close();
    parafoil_sol->read_switch();

    // initialize servos, if we're in flight state 0 we'll test them as well
    pilot->servo_init();
    if (flight_state == 0)
    {
        pilot->servo_test(); // rotates and resets each servo
        delay(200);
    }

    // initialize sensors, then indicate if we were successful or not
    if (environment->init(flight_state))
    { // if the initialization was successful and we're in flight state 0 blink 5 times
        if (flight_state == 0)
        {
            for (int i = 0; i < 5; i++)
                blink_led(500);
        }
    }
    else
    { // if the initialization was unsucessful and we're in flight state 1 blink 15 times
        if (flight_state == 0)
        {
            for (int i = 0; i < 15; i++)
            {
                analogWrite(BZZ_DTA, 200);
                blink_led(500);
                analogWrite(BZZ_DTA, 0);
            }
        }
    }
}

/* 
 * blink_led toggles the LED, then delays for a certain length of time.
 *  Can be used to achieve a blink rate, but will delay the entire execution.
 */
void Raptor::blink_led(int length)
{
    digitalWrite(LEDS_DTA, !digitalRead(LEDS_DTA));
    delay(length);
}
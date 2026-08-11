Feature: Keeping the settings the machine was tuned with

  A printer is calibrated once and then trusted. The numbers that calibration produces —
  how many steps make a millimetre, how fast each axis may go, how hard it may
  accelerate — are what stand between a dimensionally accurate part and a ruined one, and
  the person who set them expects to find them still there next time.

  So the machine has to do three separate things with them, and be clear about which it is
  doing: change them when asked, keep them across a power cycle when told to, and put them
  back to how the firmware was built when the user wants a known state again.

  Background:
    Given the printer is connected and idle

  Scenario: Changing a setting and seeing it take
    When the host sets the X steps per millimetre to 123.25
    Then the settings report gives the X steps per millimetre as 123.25

  Scenario: Changing one setting leaves the others alone
    Given the host sets the X steps per millimetre to 123.25
    When the host sets the maximum Y feedrate to 137.5
    Then the settings report gives the X steps per millimetre as 123.25
    And the settings report gives the maximum Y feedrate as 137.5

  Scenario: Going back to how the firmware was built
    Given the host sets the X steps per millimetre to 123.25
    When the host restores the factory settings
    Then the settings report no longer gives the X steps per millimetre as 123.25

  Scenario: Settings kept across a power cycle
    Given the host sets the X steps per millimetre to 123.25
    And the host saves the settings
    When the printer is restarted
    Then the settings report gives the X steps per millimetre as 123.25

  Scenario: A change that was never saved does not survive
    Given the settings have been saved as the firmware built them
    And the host sets the X steps per millimetre to 123.25
    When the printer is restarted
    Then the settings report no longer gives the X steps per millimetre as 123.25

  Scenario: Restoring the factory settings does not discard what was saved
    Given the host sets the X steps per millimetre to 123.25
    And the host saves the settings
    When the host restores the factory settings
    And the printer is restarted
    Then the settings report gives the X steps per millimetre as 123.25

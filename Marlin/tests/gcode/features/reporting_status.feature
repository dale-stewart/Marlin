Feature: Telling the host what the printer is doing

  A host draws its temperature graph, position readout and feature list from what the
  printer reports, so these replies are what the person watching actually sees.

  Background:
    Given the printer is connected and idle

  Scenario: Reporting temperatures
    Given the hotend is already at 123 degrees
    When the host asks for temperatures
    Then the reply gives the hotend temperature as 123
    And the reply gives a bed temperature

  Scenario: Reporting position
    When the host asks where the tool is
    Then the reply gives a position for every axis

  Scenario: Reporting what the firmware can do
    When the host asks what the firmware is
    Then the reply names Marlin

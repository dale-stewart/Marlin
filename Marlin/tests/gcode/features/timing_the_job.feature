Feature: Timing a print job

  A host draws its progress bar from how long the current job has been running, and a
  finished job's duration is part of what the person at the machine wants to know when it
  is done. The printer keeps a clock for the job: running while it prints, held while
  paused, and stopped once the job ends or is abandoned.

  Background:
    Given the printer is connected and idle

  Scenario: Starting a job begins timing it
    When the host starts the print job timer
    Then the job is reported to have been running for about 0 seconds

  Scenario: The clock keeps time while the job runs
    Given the host has started the print job timer
    When 5 seconds pass
    Then the job is reported to have been running for at least 4 seconds

  Scenario: Pausing the job holds the elapsed time
    Given the host has started the print job timer
    And 5 seconds pass
    When the host pauses the print job timer
    And 5 more seconds pass
    Then the job is still reported to have been running for about 5 seconds

  Scenario: Resuming a paused job carries on from where it left off
    Given the host has started the print job timer
    And 5 seconds pass
    And the host has paused the print job timer
    When the host starts the print job timer again
    And 5 more seconds pass
    Then the job is reported to have been running for at least 9 seconds

  Scenario: Stopping the job ends the timer for good
    Given the host has started the print job timer
    And 5 seconds pass
    When the host stops the print job timer
    And 5 more seconds pass
    Then the job is still reported to have been running for about 5 seconds

  Scenario: Starting a job that is already running does not throw away the time so far
    Given the host has started the print job timer
    And 5 seconds pass
    When the host starts the print job timer again
    Then the job is still reported to have been running for at least 5 seconds

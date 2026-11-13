"""Ctrl+Z, fg and bg, in a pseudo-terminal.

Ctrl+Z is pressed only once the command runs in front (see in_front).
The other stops come from the job itself with kill -STOP. They happen at a
known point.
"""

PROMPT = r"acsh\$ "
STOPS = "sh -c 'kill -STOP $$; echo went on'"


def say(shell, line):
    """Sends line and returns what it printed, up to the next prompt."""
    shell.sendline(line)
    shell.expect_exact(line + "\r\n")
    shell.expect(PROMPT)
    return shell.before


def stop_with_ctrl_z(shell, line, job_text, in_front):
    shell.sendline(line)
    in_front(shell, "sleep")
    shell.sendcontrol("z")
    shell.expect_exact(job_text + "\r\n")
    shell.expect(PROMPT)


def test_ctrl_z_stops_the_command(prompt, in_front):
    stop_with_ctrl_z(prompt, "sleep 30", "job 1 stopped: sleep 30", in_front)
    assert say(prompt, "echo $?").startswith("0\r\n")
    assert "job 1  stopped  sleep 30" in say(prompt, "jobs")


def test_fg_brings_it_back_and_ctrl_z_stops_it_again(prompt, in_front):
    stop_with_ctrl_z(prompt, "sleep 30", "job 1 stopped: sleep 30", in_front)
    prompt.sendline("fg")
    prompt.expect_exact("fg\r\nsleep 30\r\n")  # fg prints the command, like bash
    in_front(prompt, "sleep")
    prompt.sendcontrol("z")
    prompt.expect_exact("job 1 stopped: sleep 30")  # the same number again
    prompt.expect(PROMPT)
    prompt.sendline("fg %1")
    prompt.expect_exact("sleep 30\r\n")
    in_front(prompt, "sleep")
    prompt.sendintr()
    prompt.expect(PROMPT)
    assert "job" not in say(prompt, "jobs")


def test_a_whole_pipeline_stops_and_goes_on(prompt, in_front):
    stop_with_ctrl_z(prompt, "sleep 30 | cat", "job 1 stopped: sleep 30 | cat", in_front)
    assert "job 1  stopped  sleep 30 | cat" in say(prompt, "jobs")
    assert "job 1 running: sleep 30 | cat" in say(prompt, "bg")
    assert "job 1  running  sleep 30 | cat" in say(prompt, "jobs")


def test_ctrl_z_skips_the_rest_of_the_line(prompt, in_front):
    # as Ctrl+C does: only the sleep becomes a job, the loop is gone
    stop_with_ctrl_z(prompt, "while :; do sleep 30; done; echo after",
                     "job 1 stopped: sleep 30", in_front)
    assert "after" not in prompt.before


def test_bg_lets_a_stopped_job_go_on(prompt):
    prompt.sendline(STOPS)
    prompt.expect_exact(f"job 1 stopped: {STOPS}")
    prompt.expect(PROMPT)
    prompt.sendline("bg")
    prompt.expect_exact(f"job 1 running: {STOPS}\r\n")
    prompt.expect_exact("went on\r\n")
    prompt.sendline("")  # the notice comes before the next prompt
    prompt.expect_exact(f"job 1 done: {STOPS} (exit 0)")


def test_the_current_job_is_the_one_stopped_last(prompt):
    say(prompt, "sleep 30 &")
    prompt.sendline("sh -c 'kill -STOP $$; sleep 30'")
    prompt.expect_exact("job 2 stopped")
    prompt.expect(PROMPT)
    say(prompt, "sleep 30 &")
    # job 3 is newer. job 2 is the stopped one.
    assert "job 2 running" in say(prompt, "bg")
    # now job 2 went to the background last, and job 3 before it
    assert "acsh: bg: job 3 is already running" in say(prompt, "bg %-")
    assert say(prompt, "echo $?").startswith("0\r\n")  # like bash


def test_terminal_modes_go_with_the_job(prompt):
    line = "sh -c 'stty -echo; kill -STOP $$; stty'"
    prompt.sendline(line)
    prompt.expect_exact("job 1 stopped")
    prompt.expect(PROMPT)
    # the shell's modes at the prompt: what's typed is echoed again
    prompt.sendline("echo visible")
    prompt.expect_exact("echo visible\r\nvisible\r\n")
    prompt.expect(PROMPT)
    # and the job's own when it's back: stty shows -echo
    prompt.sendline("fg")
    prompt.expect("-echo")
    prompt.expect(PROMPT)


def test_fg_on_a_job_that_ended_gives_its_status(prompt):
    prompt.sendline("sh -c 'exit 3' & sleep 0.5; fg")
    prompt.expect_exact("job 1 done: sh -c 'exit 3' (exit 3)")
    prompt.expect(PROMPT)
    assert say(prompt, "echo $?").startswith("3\r\n")


def test_fg_and_bg_errors(prompt):
    assert "acsh: fg: no current job" in say(prompt, "fg")
    say(prompt, "sleep 30 &")
    assert "acsh: fg: %5: no such job" in say(prompt, "fg %5")


def test_fg_and_bg_in_a_script(run):
    result = run(stdin="fg\necho $?\nbg\necho $?\n")
    assert result.stdout == "1\n1\n"
    assert result.stderr == ("acsh: fg: no job control in this shell\n"
                             "acsh: bg: no job control in this shell\n")

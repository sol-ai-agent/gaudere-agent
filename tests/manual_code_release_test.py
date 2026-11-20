#!/usr/bin/env python3
"""Isolated manual release tests: fake Podman/systemd, no host or network effects."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'deploy-agent-code-only-manual.sh'
OLD = 'sha256:' + 'a' * 64
NEW = 'sha256:' + 'b' * 64
AGENT = 'c' * 40
CORE = 'd' * 40


def write_executable(path: Path, data: str) -> None:
    path.write_text('#!/usr/bin/env bash\nset -euo pipefail\n' + data)
    path.chmod(0o755)


class ManualCodeReleaseTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='gaudere-manual-release-test-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.home = self.root / 'home'
        self.bin = self.root / 'bin'
        self.checkout = self.root / 'checkout'
        self.settings = self.home / '.config/containers/systemd'
        self.state = self.home / '.local/share/gaudere/state'
        for path in (self.bin, self.checkout / 'scripts', self.settings, self.state):
            path.mkdir(parents=True, exist_ok=True)
        (self.state / 'state.db').write_text('durable-data')
        (self.state / 'dialogue.db').write_text('dialogue-data')
        self.quadlet = self.settings / 'gaudere-agent.container'
        self.original_quadlet = (
            '[Container]\n'
            f'Image={OLD}\n'
            'Network=none\n'
            'Volume=%h/.local/share/gaudere/state:/var/lib/gaudere:Z\n'
        )
        self.quadlet.write_text(self.original_quadlet)
        self.quadlet.chmod(0o600)
        (self.root / 'service').write_text('active')
        (self.root / 'running_image').write_text(OLD)
        (self.root / 'actions').write_text('')
        (self.checkout / 'gaudere.ref').write_text(CORE + '\n')
        write_executable(self.bin / 'git', f'''
if [[ "$1" == -C && "$3" == rev-parse ]]; then echo '{AGENT}'; exit; fi
if [[ "$1" == -C && "$3" == status ]]; then exit; fi
exit 99
''')
        # Only the Quadlet staging script calls python3; inject an early failure.
        write_executable(self.bin / 'python3', f'''
if [[ "${{TEST_FAIL_STAGE:-}}" == 1 ]]; then exit 71; fi
exec '{sys.executable}' "$@"
''')
        write_executable(self.bin / 'systemctl', '''
[[ "$1" == --user ]] || exit 99
shift
case "$1" in
 is-active) cat "$TEST_ROOT/service" ;;
 stop) echo stop >> "$TEST_ROOT/actions"; printf 'inactive' > "$TEST_ROOT/service" ;;
 daemon-reload) echo daemon-reload >> "$TEST_ROOT/actions" ;;
 start)
   echo start >> "$TEST_ROOT/actions"
   image=$(sed -n 's/^Image=//p' "$TEST_QUADLET")
   if [[ "${TEST_FAIL_START:-}" == 1 && "$image" == "$GAUDERE_MANUAL_NEW_IMAGE" ]]; then exit 99; fi
   printf '%s' active > "$TEST_ROOT/service"
   printf '%s' "$image" > "$TEST_ROOT/running_image"
   if [[ "${TEST_MUTATE_NEW:-}" == 1 && "$image" == "$GAUDERE_MANUAL_NEW_IMAGE" ]]; then
     printf '%s' changed > "$HOME/.local/share/gaudere/state/state.db"
   fi ;;
 *) exit 99 ;;
esac
''')
        write_executable(self.bin / 'podman', '''
case "$1:$2" in
 'inspect:gaudere-agent')
    case "${4:-}" in
      '{{.Image}}') cat "$TEST_ROOT/running_image" ;;
      '{{.HostConfig.NetworkMode}}') echo none ;;
      '{{.State.Status}}')
         [[ "$(cat "$TEST_ROOT/service")" == active ]] && echo running || echo exited ;;
      *) exit 99 ;;
    esac ;;
 'image:inspect')
     [[ "$3" == "$GAUDERE_MANUAL_NEW_IMAGE" ]] || exit 98
     printf '%s\\n' "$GAUDERE_MANUAL_NEW_IMAGE" ;;
 'run:--rm')
     echo 'Usage: /usr/local/bin/gaudere-chat [--io-timeout-ms N]' >&2 ;;
 'exec:gaudere-agent')
     echo 'Usage: gaudere-chat [--io-timeout-ms N]' >&2 ;;
 *) echo "unsupported fake podman args: $*" >&2; exit 99 ;;
esac
''')
        write_executable(self.bin / 'sqlite3', '''
[[ "$1" == -readonly && "$3" == .dump ]] || exit 99
cat "$2"
''')
        write_executable(self.checkout / 'scripts' / 'backup-state.sh', '''
root=${GAUDERE_STATE_DIR:?}
[ "${TEST_BACKUP_FAIL:-}" != 1 ] || exit 99
back="$HOME/.local/share/gaudere/backups"
mkdir -p "$back"
archive="$back/test-state.tar.gz"
tar -czf "$archive" -C "$root" .
(cd "$back" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")
printf '%s\\n' "$archive"
''')
        write_executable(self.checkout / 'scripts' / 'verify-image-provenance.sh', '''
[ "$1" = "$GAUDERE_MANUAL_NEW_IMAGE" ] && [ "$2" = "$GAUDERE_MANUAL_AGENT_REF" ] && [ "$3" = "$GAUDERE_MANUAL_CORE_REF" ] && [ "$4" = "$GAUDERE_MANUAL_NEW_IMAGE" ] || exit 98
echo 'image_provenance=PASS'
''')
        self.env = dict(os.environ, HOME=str(self.home), TEST_ROOT=str(self.root),
            TEST_QUADLET=str(self.quadlet),
            PATH=f'{self.bin}:' + os.environ.get('PATH', ''),
            GAUDERE_MANUAL_CHECKOUT=str(self.checkout),
            GAUDERE_MANUAL_OLD_IMAGE=OLD, GAUDERE_MANUAL_NEW_IMAGE=NEW,
            GAUDERE_MANUAL_AGENT_REF=AGENT, GAUDERE_MANUAL_CORE_REF=CORE)
        self.env.pop('GAUDERE_MANUAL_RELEASE_ACK', None)

    def run_script(self, *args, env=None):
        return subprocess.run(['bash', str(SCRIPT), *args],
            env=env or self.env, capture_output=True, text=True, timeout=30)

    def assert_old_running(self):
        self.assertEqual((self.root / 'service').read_text(), 'active')
        self.assertEqual((self.root / 'running_image').read_text(), OLD)
        self.assertEqual(self.quadlet.read_text(), self.original_quadlet)

    def test_default_preflight_never_stops_agent(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('RELEASE=PRECHECK_PASS', result.stdout)
        self.assertEqual((self.root / 'actions').read_text(), '')
        self.assert_old_running()

    def test_apply_requires_ack_before_touching_host(self):
        result = self.run_script('--apply')
        self.assertNotEqual(result.returncode, 0)
        self.assert_old_running()
        self.assertEqual((self.root / 'actions').read_text(), '')

    def test_bad_quadlet_blocks_before_stop(self):
        self.quadlet.write_text(self.original_quadlet.replace('Network=none', 'Network=host'))
        result = self.run_script('--apply', env=dict(self.env,
            GAUDERE_MANUAL_RELEASE_ACK='APPLY_GAUDERE_AGENT_CODE_ONLY'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('BLOCKED_BEFORE_STOP', result.stderr)
        self.assertEqual((self.root / 'actions').read_text(), '')

    def test_apply_switches_only_image_and_preserves_databases(self):
        result = self.run_script('--apply', env=dict(self.env,
            GAUDERE_MANUAL_RELEASE_ACK='APPLY_GAUDERE_AGENT_CODE_ONLY'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('RELEASE=PASS', result.stdout)
        self.assertEqual((self.root / 'running_image').read_text(), NEW)
        self.assertEqual(self.quadlet.read_text(), self.original_quadlet.replace(OLD, NEW))
        self.assertEqual((self.state / 'state.db').read_text(), 'durable-data')
        self.assertTrue((self.home / '.local/share/gaudere/backups/test-state.tar.gz.sha256').exists())

    def test_stage_failure_cleans_only_owned_temp_and_never_stops(self):
        result = self.run_script('--apply', env=dict(self.env,
            GAUDERE_MANUAL_RELEASE_ACK='APPLY_GAUDERE_AGENT_CODE_ONLY',
            TEST_FAIL_STAGE='1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('BLOCKED_BEFORE_STOP', result.stderr)
        self.assert_old_running()
        self.assertEqual((self.root / 'actions').read_text(), '')
        self.assertEqual(list(self.settings.glob('gaudere-agent.container.manual-release.*')), [])

    def test_existing_legacy_temp_is_not_removed_by_release(self):
        foreign = self.settings / 'gaudere-agent.container.manual-release.tmp'
        foreign.write_text('somebody-elses-content')
        result = self.run_script('--apply', env=dict(self.env,
            GAUDERE_MANUAL_RELEASE_ACK='APPLY_GAUDERE_AGENT_CODE_ONLY',
            TEST_FAIL_STAGE='1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(foreign.read_text(), 'somebody-elses-content')
        self.assert_old_running()
        self.assertEqual((self.root / 'actions').read_text(), '')

    def test_failed_backup_restores_original_service(self):
        result = self.run_script('--apply', env=dict(self.env,
            GAUDERE_MANUAL_RELEASE_ACK='APPLY_GAUDERE_AGENT_CODE_ONLY',
            TEST_BACKUP_FAIL='1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ROLLBACK=PASS', result.stderr)
        self.assert_old_running()

    def test_candidate_state_mutation_restores_exact_backup(self):
        result = self.run_script('--apply', env=dict(self.env,
            GAUDERE_MANUAL_RELEASE_ACK='APPLY_GAUDERE_AGENT_CODE_ONLY',
            TEST_MUTATE_NEW='1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ROLLBACK=PASS', result.stderr)
        self.assert_old_running()
        self.assertEqual((self.state / 'state.db').read_text(), 'durable-data')

    def test_failed_candidate_start_recovers_original_service(self):
        result = self.run_script('--apply', env=dict(self.env,
            GAUDERE_MANUAL_RELEASE_ACK='APPLY_GAUDERE_AGENT_CODE_ONLY',
            TEST_FAIL_START='1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ROLLBACK=PASS', result.stderr)
        self.assert_old_running()
        self.assertEqual((self.state / 'state.db').read_text(), 'durable-data')


if __name__ == '__main__':
    unittest.main()

"""Offline account runner/stream semantics, including real generated adapters.

Run under an isolated network namespace. No accounts, live provider, audio,
microphone or device are used. Fake transport is below the real shell provider
and its real C SSE normalizer, not a copy of the normalized-output producer.
"""
import ctypes as C
import errno
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent
ADAPTER = ROOT.parent / 'model-adapter'


class Tag(C.Structure):
    _fields_ = [('session_epoch', C.c_uint64), ('reply_generation', C.c_uint64),
                ('utterance_id', C.c_uint64)]


class Event(C.Structure):
    _fields_ = [('type', C.c_int), ('tag', Tag), ('segment_sequence', C.c_uint64),
                ('at_ms', C.c_uint64), ('error', C.c_int), ('length', C.c_size_t),
                ('text', C.c_char * 2049)]


def now():
    return int(time.monotonic() * 1000)


def text(event):
    return bytes(event.text)[:event.length].decode('utf-8')


def snapshot(event):
    return dict(type=event.type, tag=(event.tag.session_epoch, event.tag.reply_generation,
                                     event.tag.utterance_id),
                sequence=event.segment_sequence, at=event.at_ms,
                error=event.error, text=text(event))


class AccountTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='dc-account-build-')
        cls.libpath = Path(cls.build.name) / 'account.so'
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-shared', '-fPIC', str(ROOT / 'account.c'),
                        '-o', str(cls.libpath)], check=True)
        cls.lib = C.CDLL(str(cls.libpath), use_errno=True)
        cls.lib.dc_account_create.restype = C.c_void_p
        cls.lib.dc_account_destroy.argtypes = [C.c_void_p]
        cls.lib.dc_account_start.argtypes = [C.c_void_p, Tag, C.c_char_p, C.c_char_p,
                                             C.c_char_p, C.c_uint64]
        cls.lib.dc_account_cancel.argtypes = [C.c_void_p, Tag, C.c_uint64]
        cls.lib.dc_account_tick.argtypes = [C.c_void_p, C.c_uint64]
        for name in ('poll', 'phrase'):
            getattr(cls.lib, 'dc_account_' + name).argtypes = [C.c_void_p, C.c_uint64, C.POINTER(Event)]
        for name in ('busy', 'drained', 'retired', 'fd', 'write_fd', 'history_count'):
            getattr(cls.lib, 'dc_account_' + name).argtypes = [C.c_void_p]
        cls.lib.dc_account_storage_bytes.restype = C.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='dc-account-test-')
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.a = self.lib.dc_account_create()
        self.assertTrue(self.a)
        self.tag = Tag(1, 1, 1)
        self.script('request=json.load(sys.stdin)\n'
                    '(root/"input").write_text(json.dumps(request,ensure_ascii=False))\n'
                    'print(json.dumps({"delta":"这是完整回答。"},ensure_ascii=False),flush=True)\n'
                    'print("{\\"done\\":true}",flush=True)\n')
        self.addCleanup(self.shutdown)

    def shutdown(self):
        if not self.a:
            return
        self.lib.dc_account_cancel(self.a, self.tag, now())
        until = time.monotonic() + 1
        while self.lib.dc_account_retired(self.a) and time.monotonic() < until:
            self.lib.dc_account_tick(self.a, now())
            time.sleep(.005)
        self.assertEqual(self.lib.dc_account_destroy(self.a), 0)
        self.a = None

    def script(self, body):
        impl = self.path / 'request.py'
        impl.write_text('import os,sys,json,time,signal,subprocess\nfrom pathlib import Path\n'
                        + 'root=Path(' + repr(str(self.path)) + ')\n' + body)
        self.scriptpath = self.path / 'request.sh'
        self.scriptpath.write_text('#!/bin/sh\nexec python3 ' + str(impl) + ' "$@"\n')

    def wire(self, records, exit_code=0, delay=0):
        self.script('json.load(sys.stdin)\n'
                    + 'data=' + repr(records) + '\n'
                    + 'sys.stdout.buffer.write(data);sys.stdout.buffer.flush()\n'
                    + 'time.sleep(' + repr(delay) + ')\n'
                    + 'sys.exit(' + repr(exit_code) + ')\n')

    def start(self, question='请用两句话介绍北京故宫。', model='selected-model', epoch=1,
              generation=1, utterance=1, script=None):
        self.tag = Tag(epoch, generation, utterance)
        return self.lib.dc_account_start(self.a, self.tag, question.encode(),
                                         str(script or self.scriptpath).encode(), model.encode(), now())

    def drain(self, timeout=5, pull=True):
        events = []
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            ev = Event()
            rc = self.lib.dc_account_poll(self.a, now(), C.byref(ev))
            self.assertGreaterEqual(rc, 0)
            if rc:
                events.append(snapshot(ev))
            if pull:
                ev = Event()
                rc = self.lib.dc_account_phrase(self.a, now(), C.byref(ev))
                self.assertGreaterEqual(rc, 0)
                if rc:
                    events.append(snapshot(ev))
            if not self.lib.dc_account_busy(self.a) and (not pull or self.lib.dc_account_drained(self.a)):
                return events
            time.sleep(.001)
        self.fail('account runner exceeded bounded fixture deadline')

    def terminal(self, events):
        return [e for e in events if e['type'] in (3, 4)]

    def test_complete_final_request_and_history_preserve_selected_model(self):
        question = '请用两句话介绍北京故宫，后半句必须完整。'
        self.assertEqual(self.start(question=question, model='user-selected-model'), 0)
        events = self.drain()
        self.assertEqual([e['type'] for e in self.terminal(events)], [3])
        first = json.loads((self.path / 'input').read_text())
        self.assertEqual(first, {'values': {'model': 'user-selected-model'},
                                 'input': [{'role': 'user', 'content': question}]})
        self.assertEqual(self.lib.dc_account_history_count(self.a), 1)
        self.assertEqual(self.start(question='继续。', generation=2, utterance=2), 0)
        self.drain()
        request = json.loads((self.path / 'input').read_text())
        self.assertEqual([x['role'] for x in request['input']], ['user', 'assistant', 'user'])
        self.assertEqual(request['input'][0]['content'], question)
        self.assertEqual(request['input'][1]['content'], '这是完整回答。')

    def test_fragmented_json_decodes_escapes_surrogates_and_tags_every_event(self):
        answer = '北京故宫，回答中有"delta"和\\。🙂\n下一句。'
        wire = (json.dumps({'delta': answer}) + '\r\n{"done":1}\n').encode()
        self.script('json.load(sys.stdin)\n'
                    + 'data=' + repr(wire) + '\n'
                    + 'for part in (data[:5],data[5:29],data[29:48],data[48:]):\n'
                    + ' sys.stdout.buffer.write(part);sys.stdout.buffer.flush();time.sleep(.005)\n')
        self.assertEqual(self.start(epoch=17, generation=23, utterance=31), 0)
        events = self.drain()
        self.assertEqual(''.join(e['text'] for e in events if e['type'] == 1), answer)
        self.assertEqual(''.join(e['text'] for e in events if e['type'] == 2), answer)
        self.assertTrue(all(e['tag'] == (17, 23, 31) for e in events))

    def test_backpressure_keeps_text_until_tts_slot_and_sequence_is_bounded(self):
        answer = '甲乙丙丁戊己庚辛壬癸🙂' * 90 + '。'
        self.wire((json.dumps({'delta': answer[:300]}, ensure_ascii=False) + '\n'
                   + json.dumps({'delta': answer[300:600]}, ensure_ascii=False) + '\n'
                   + json.dumps({'delta': answer[600:]}, ensure_ascii=False) + '\n{"done":true}\n').encode())
        # Keep each normalized delta within the real producer's 2048 B bound.
        self.script('json.load(sys.stdin)\nanswer=' + repr(answer) + '\n'
                    'for at in range(0,len(answer),100):\n'
                    ' print(json.dumps({"delta":answer[at:at+100]},ensure_ascii=False),flush=True)\n'
                    'print("{\\"done\\":true}",flush=True)\n')
        self.assertEqual(self.start(), 0)
        events = self.drain(pull=False)
        self.assertEqual([e['type'] for e in self.terminal(events)], [3])
        self.assertFalse(self.lib.dc_account_drained(self.a))
        phrases = []
        while not self.lib.dc_account_drained(self.a):
            event = Event()
            self.assertEqual(self.lib.dc_account_phrase(self.a, now(), C.byref(event)), 1)
            phrases.append(snapshot(event))
        self.assertEqual(''.join(e['text'] for e in phrases), answer)
        self.assertEqual([e['sequence'] for e in phrases], list(range(1, len(phrases) + 1)))
        self.assertLessEqual(len(phrases[0]['text'].encode()), 72)
        self.assertTrue(all(len(e['text'].encode()) <= 240 for e in phrases))
        self.assertLessEqual(self.lib.dc_account_storage_bytes(), 80 * 1024)

    def test_phrase_available_before_done_and_sparse_wait_is_250ms(self):
        self.wire('{"delta":"这是一段没有句号但必须尽快播出的文字"}\n'.encode(), delay=.7)
        self.assertEqual(self.start(), 0)
        events = self.drain()
        phrase = next(e for e in events if e['type'] == 2)
        delta = next(e for e in events if e['type'] == 1)
        self.assertGreaterEqual(phrase['at'] - delta['at'], 250)
        self.assertLess(phrase['at'] - delta['at'], 400)
        self.assertEqual(self.terminal(events)[0]['type'], 4)  # no done -> no history
        self.assertEqual(self.lib.dc_account_history_count(self.a), 0)

    def test_malformed_records_do_not_commit_history_or_publish_raw_errors(self):
        cases = [b'{"delta":"ok","done":true}\n', b'{"delta":"one","delta":"two"}\n',
                 b'{"done":false}\n', b'{"done":1.0}\n', b'{"done":true}\n',
                 b'{"delta":"\\ud800"}\n', b'{"delta":"\\udc00"}\n',
                 b'{"delta":"\\u0000"}\n', b'{"delta":"\xc0\xaf"}\n',
                 b'{"delta":"unfinished"}', b'\n', b'{"delta":"x"}\n{"done":true}\n{}\n',
                 b'{"error":"bad\\nnotice"}\n', b'{"error":""}\n',
                 b'{"delta":"secret suffix",BROKEN}\n']
        for generation, wire in enumerate(cases, 1):
            with self.subTest(wire=wire):
                self.wire(wire)
                self.assertEqual(self.start(generation=generation, utterance=generation), 0)
                events = self.drain()
                self.assertEqual([e['type'] for e in self.terminal(events)], [4])
                self.assertEqual(self.terminal(events)[0]['text'], '')
                self.assertEqual(self.lib.dc_account_history_count(self.a), 0)
                # Retire promptly without waiting for real grace in every case.
                self.lib.dc_account_tick(self.a, now() + 81)
                for _ in range(20):
                    self.lib.dc_account_tick(self.a, now() + 81)
                    if not self.lib.dc_account_retired(self.a):
                        break
                    time.sleep(.002)

    def test_true_error_and_existing_public_message_are_errors(self):
        for generation, record in enumerate(({'error': True}, {'error': '平台拒绝了此账户。'},
                                               {'message': '请先登录账户。'}), 1):
            self.wire((json.dumps(record, ensure_ascii=False) + '\n').encode())
            self.assertEqual(self.start(generation=generation, utterance=generation), 0)
            event = self.terminal(self.drain())[0]
            self.assertEqual((event['type'], event['error']), (4, 4))
            self.assertEqual(event['text'], (record['message'] if 'message' in record else record['error'])
                             if record.get('error') is not True else '')

    def test_done_requires_clean_exit_and_clean_exit_requires_done(self):
        for generation, (tail, exit_code) in enumerate(((b'{"done":true}\n', 7), (b'', 0)), 1):
            self.wire(b'{"delta":"partial answer"}\n' + tail, exit_code=exit_code)
            self.assertEqual(self.start(generation=generation, utterance=generation), 0)
            self.assertEqual([e['type'] for e in self.terminal(self.drain())], [4])
            self.assertEqual(self.lib.dc_account_history_count(self.a), 0)

    def test_answer_and_line_limits_fail_without_partial_history(self):
        self.wire(b'{"delta":"' + b'x' * 4096 + b'"}\n')
        self.assertEqual(self.start(), 0)
        self.assertEqual(self.terminal(self.drain())[0]['error'], 3)
        self.script('json.load(sys.stdin)\n'
                    'for _ in range(17): print(json.dumps({"delta":"x"*1024}),flush=True)\n'
                    'print("{\\"done\\":true}",flush=True)\n')
        self.assertEqual(self.start(generation=2, utterance=2), 0)
        self.assertEqual(self.terminal(self.drain())[0]['error'], 3)
        self.assertEqual(self.lib.dc_account_history_count(self.a), 0)

    def test_cancel_is_immediate_idempotent_and_late_reply_cannot_touch_new_one(self):
        self.wire(b'{"delta":"old answer pending"}\n', delay=2)
        self.assertEqual(self.start(), 0)
        until = time.monotonic() + 2
        while time.monotonic() < until:
            event = Event()
            if self.lib.dc_account_poll(self.a, now(), C.byref(event)):
                self.assertEqual(event.type, 1)
                break
            time.sleep(.001)
        self.assertEqual(self.lib.dc_account_cancel(self.a, Tag(9, 1, 1), now()), 0)
        began = time.monotonic()
        self.assertEqual(self.lib.dc_account_cancel(self.a, self.tag, now()), 1)
        self.assertLess(time.monotonic() - began, .02)
        self.assertEqual(self.lib.dc_account_cancel(self.a, self.tag, now()), 0)
        self.assertFalse(self.lib.dc_account_busy(self.a))
        self.assertTrue(self.lib.dc_account_drained(self.a))
        self.assertEqual(self.lib.dc_account_history_count(self.a), 0)
        old = self.tag
        self.wire(b'{"delta":"new complete answer"}\n{"done":true}\n')
        self.assertEqual(self.start(question='等一下后面完整的新问题。', generation=2, utterance=1), 0)
        self.assertEqual(self.lib.dc_account_cancel(self.a, old, now()), 0)
        events = self.drain()
        self.assertEqual([e['type'] for e in self.terminal(events)], [3])
        self.assertTrue(all(e['tag'] == (1, 2, 1) for e in events))
        self.assertEqual(''.join(e['text'] for e in events if e['type'] == 2), 'new complete answer')

    def test_destroy_busy_and_bounded_cancel_backlog(self):
        self.wire(b'', delay=2)
        for generation in range(1, 9):
            self.assertEqual(self.start(generation=generation, utterance=generation), 0)
            self.assertEqual(self.lib.dc_account_destroy(self.a), -1)
            self.assertEqual(C.get_errno(), errno.EBUSY)
            self.assertEqual(self.lib.dc_account_cancel(self.a, self.tag, now()), 1)
        self.assertEqual(self.lib.dc_account_retired(self.a), 8)
        self.assertEqual(self.start(generation=9, utterance=9), -1)
        self.assertEqual(C.get_errno(), errno.EBUSY)
        self.lib.dc_account_tick(self.a, now() + 81)

    def test_invalid_inputs_are_rejected_and_epoch_resets_history(self):
        self.assertEqual(self.start(question='x' * 2048), -1)
        self.assertEqual(C.get_errno(), errno.EMSGSIZE)
        self.assertEqual(self.start(question=''), -1)
        self.assertEqual(self.start(utterance=0), -1)
        link = self.path / 'link.sh'
        link.symlink_to(self.scriptpath)
        self.assertEqual(self.start(script=link), -1)
        self.assertEqual(self.start(), 0)
        self.drain()
        self.assertEqual(self.start(), -1)  # identical reply tag cannot restart
        self.assertEqual(self.start(epoch=2), 0)
        self.drain()
        self.assertEqual(len(json.loads((self.path / 'input').read_text())['input']), 1)
        self.assertEqual(self.lib.dc_account_history_count(self.a), 1)

    def test_early_adapter_exit_cannot_sigpipe_controller(self):
        self.script('sys.stdin.close()\nsys.exit(7)\n')
        self.assertEqual(self.start(), 0)
        self.assertEqual([e['type'] for e in self.terminal(self.drain())], [4])

    def test_three_pair_history_rolls_and_json_quotes_input(self):
        for generation in range(1, 6):
            self.assertEqual(self.start(question='question "' + str(generation) + '"\n\\',
                                        generation=generation, utterance=generation), 0)
            self.drain()
        request = json.loads((self.path / 'input').read_text())
        self.assertEqual(len(request['input']), 7)
        self.assertIn('"2"', request['input'][0]['content'])
        self.assertEqual(self.lib.dc_account_history_count(self.a), 3)

    def test_real_generated_grok_chatgpt_model_request_and_sse_normalizer(self):
        sys.path.insert(0, str(ADAPTER))
        try:
            spec = importlib.util.spec_from_file_location('account_existing_fixture', ADAPTER / 'test_accounts.py')
            fixture = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(fixture)
            scripts = self.path / 'scripts'
            fixture.builder.build(scripts)
            tool = self.path / 'model-tool'
            subprocess.run(['gcc', '-O2', str(ADAPTER / 'model-tool.c'), '-lcrypto', '-o', str(tool)], check=True)
            accounts = self.path / 'accounts'
            for provider in ('grok', 'chatgpt'):
                file = scripts / (provider + '_account.sh')
                body = file.read_text().replace('TOOL=/usr/bin/lx06-model-tool', 'TOOL=' + str(tool))
                body = body.replace('ROOT=/data/lx06/model-accounts', 'ROOT=' + str(accounts))
                body = body.replace('[ "$(id -u)" = 0 ]', '[ 0 = 0 ]')
                file.write_text(body)
                record = accounts / provider
                record.mkdir(parents=True)
                (record / 'credentials.json').write_text(json.dumps(dict(
                    access_token='fake-access', refresh_token='fake-refresh', expires_at=str(int(time.time()) + 3600),
                    scope='openid offline_access', client_id='fixture-client', subject='fixture-subject')))
            binpath = self.path / 'bin'
            binpath.mkdir()
            curl = binpath / 'curl'
            curl.write_text(fixture.FAKE_CURL)
            curl.chmod(0o700)
            (self.path / 'fixture.json').write_text('{}')
            before = dict(os.environ)
            os.environ.update(FIXTURE=str(self.path), PATH=str(binpath) + ':' + before['PATH'])
            try:
                for epoch, provider in enumerate(('grok', 'chatgpt'), 101):
                    with self.subTest(provider=provider):
                        self.assertEqual(self.start(question='请用两句话介绍北京故宫，后半句完整。',
                                                    model='user-chosen-' + provider, epoch=epoch,
                                                    script=scripts / (provider + '_account.sh')), 0)
                        events = self.drain()
                        self.assertEqual([e['type'] for e in self.terminal(events)], [3], events)
                        self.assertEqual(''.join(e['text'] for e in events if e['type'] == 2), '测试完整回答')
                        call = json.loads((self.path / 'calls').read_text().splitlines()[-1])
                        body = json.loads(call['body'])
                        self.assertEqual(body['model'], 'user-chosen-' + provider)
                        self.assertEqual(body['input'][-1]['content'], '请用两句话介绍北京故宫，后半句完整。')
                        self.assertTrue(body['stream'])
            finally:
                os.environ.clear()
                os.environ.update(before)
        finally:
            sys.path.remove(str(ADAPTER))


if __name__ == '__main__':
    unittest.main(verbosity=2)

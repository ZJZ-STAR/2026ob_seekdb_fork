#!/usr/bin/env python3
"""Deterministic provider protocols with request evidence and fault injection."""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def vector(text, dimension):
    digest = hashlib.sha256(text.encode('utf-8')).digest()
    return [round((digest[i % len(digest)] - 128) / 128, 6) for i in range(dimension)]


class Handler(BaseHTTPRequestHandler):
    lock = threading.Lock()
    sequence = 0
    attempts = {}

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.reply({'status': 'ready'})

    def reply(self, body, status=200):
        raw = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        with self.lock:
            Handler.sequence += 1
            event = {'sequence': Handler.sequence, 'path': self.path, 'body': body,
                     'content_type': self.headers.get('Content-Type'),
                     'has_authorization': bool(self.headers.get('Authorization'))}
            with self.server.evidence.open('a', encoding='utf-8') as out:
                out.write(json.dumps(event, ensure_ascii=False) + '\n')
            Handler.attempts[self.path] = Handler.attempts.get(self.path, 0) + 1
            attempt = Handler.attempts[self.path]
        mode = self.path.split('/')[2] if self.path.startswith('/fault/') else ''
        if mode == 'http-error' or (mode == 'retry-once' and attempt == 1):
            return self.reply({'error': 'injected service failure'}, 503)
        if mode == 'invalid-json':
            return self.reply(b'{invalid')
        if mode == 'root-array':
            return self.reply([])
        if mode == 'empty-body':
            return self.reply(b'')
        if self.path.endswith('/chat/completions'):
            assert isinstance(body.get('messages'), list)
            content = body['messages'][-1]['content']
            value = '' if mode == 'empty' else (None if mode == 'null' else content)
            result = {'choices': [{'message': {'content': value}}]}
            if mode == 'missing': result = {'choices': []}
            if mode == 'wrong-type': result['choices'][0]['message']['content'] = [1]
        elif self.path.endswith('/api/generate'):
            assert body['stream'] is False
            assert not self.headers.get('Authorization')
            assert self.headers.get('Content-Type') == 'application/json'
            result = {'response': '' if mode == 'empty' else body['prompt'], 'done': True}
        elif self.path.endswith('/embeddings') or self.path.endswith('/api/embed'):
            contents = body['input']
            assert isinstance(contents, list) and all(isinstance(x, str) for x in contents)
            dimension = body.get('dimensions', 768)
            if not isinstance(dimension, int) or dimension <= 0 or dimension > 65536:
                return self.reply({'error': 'unsupported mock dimension'}, 400)
            values = [vector(x, dimension) for x in contents]
            if mode == 'dimension': values[0] = values[0][:-1]
            if mode == 'zero-vector': values[0] = []
            if mode == 'bad-element': values[0][0] = 'x'
            if mode == 'null-element': values[0][0] = None
            if mode == 'bool-element': values[0][0] = True
            if mode == 'not-vector': values[0] = {'x': 1}
            if mode == 'not-finite': values[0][0] = float('inf')
            if self.path.endswith('/api/embed'):
                assert not self.headers.get('Authorization')
                result = {'embeddings': values}
                if mode == 'wrong-type': result['embeddings'] = 'bad'
                if mode == 'missing': result = {}
                if mode == 'count': result['embeddings'].pop()
            else:
                data = [{'index': i, 'embedding': value} for i, value in enumerate(values)]
                if mode == 'index': data[0]['index'] = len(data)
                if mode == 'negative': data[0]['index'] = -1
                if mode == 'float-index': data[0]['index'] = 0.5
                if mode == 'duplicate' and len(data) > 1: data[-1]['index'] = data[0]['index']
                if mode == 'no-index': del data[0]['index']
                if mode == 'count': data.pop()
                # Always reverse provider records to exercise SQL row recovery.
                result = {'data': data[::-1]}
                if mode == 'wrong-type': result['data'] = 'bad'
                if mode == 'missing': result = {}
        elif self.path.endswith('/rerank'):
            documents = body['documents']
            query = body['query']
            assert len(documents) <= 20
            scores = [{'index': i, 'relevance_score': int(query in text)} for i, text in enumerate(documents)]
            result = {'results': scores[::-1]}
            if mode == 'count': result['results'].pop()
            if mode == 'duplicate' and len(scores) > 1: scores[-1]['index'] = scores[0]['index']
            if mode == 'index': scores[0]['index'] = len(documents)
            if mode == 'negative': scores[0]['index'] = -1
            if mode == 'float-index': scores[0]['index'] = 0.2
            if mode == 'missing': del scores[0]['relevance_score']
            if mode == 'wrong-type': scores[0]['relevance_score'] = 'bad'
            if mode == 'not-finite': scores[0]['relevance_score'] = float('inf')
        else:
            return self.reply({'error': 'unknown route'}, 404)
        return self.reply(result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    server.evidence = args.evidence
    print('Mock AI Service listening on 0.0.0.0:%d' % args.port, flush=True)
    server.serve_forever()

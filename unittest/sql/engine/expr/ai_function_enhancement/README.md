# AI function enhancement regression tests

The two C++ test sources in the parent directory cover Prompt rendering and
OpenAI/Ollama provider protocols. `run_cpp_tests.py` reuses the configured
competition CMake toolchain and libraries to compile and run them.

`mock_ai_service.py` provides deterministic HTTP responses, reverses OpenAI
embedding records, records requests, and supports fault routes.
`test_ai_functions.py` runs 87 SQL integration cases. `run_mysqltests.py` runs
the existing `ai_function_enhancement.test` and `ai_prompt.test` cases.

School-server commands:

```bash
source /home/PeterPan/2026ob/env/activate.sh
python3 /home/PeterPan/2026ob/tests/ai-functions/run_cpp_tests.py
python3 /home/PeterPan/2026ob/tests/ai-functions/run_mysqltests.py
python3 /home/PeterPan/2026ob/tests/ai-functions/test_ai_functions.py
```

The test database socket is
`/home/PeterPan/2026ob/run/seekdb-ai-functions/run/sql.sock`.
The Mock service listens on port 8000. Scripts write results to
`/home/PeterPan/2026ob/results/ai-functions/`.

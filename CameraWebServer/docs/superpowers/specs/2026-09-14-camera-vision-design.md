# Camera vision recognition design

## Scope

Add optional automatic and manual image recognition to the CameraWebServer example. A browser page controls recognition and displays the latest model response; the ESP32 also prints each response to Serial.

## Architecture

- `vision.h/.cpp` owns capture, HTTPS/OpenAI-compatible request, response parsing, state, and the periodic FreeRTOS task.
- `vision_config.h` contains endpoint, model, interval defaults, and the API key supplied by the user.
- `app_httpd.cpp` registers `/vision`, `/vision/status`, `/vision/trigger`, and `/vision/config` handlers.
- The vision task is serialized with a mutex so manual and automatic captures cannot overlap.
- Captures use the camera's existing JPEG QVGA configuration. The JPEG is base64 encoded into a Chat Completions JSON request.

## Web flow

The `/vision` page polls `/vision/status`, toggles automatic mode through `/vision/config?enabled=...&interval=...`, and starts a manual request through `/vision/trigger`. Status JSON contains the latest result, request state, and settings.

## Error handling

HTTPS failures, non-2xx API responses, malformed responses, camera capture failures, and oversized allocations are reported to Serial and exposed as the latest chat message. Automatic mode continues after an error and waits for the next interval.

## Security and resource limits

The API key is kept in a local header as requested, but is compiled into firmware and should be rotated because it was exposed in chat. TLS certificate verification is disabled for compatibility with the custom endpoint; production firmware should install the provider CA certificate.

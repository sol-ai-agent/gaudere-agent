# Durable provider usage metadata

Gaudere keeps provider accounting separate from user-visible Task output.

For a definite OpenAI Responses result, the adapter may attach normalized machine-readable metadata with content type:

`application/vnd.gaudere.provider-usage+json`

The v1 document contains only Gaudere-selected fields:

```json
{
  "schema": "gaudere.provider_usage.v1",
  "provider": "openai",
  "model": "gpt-5.6-sol",
  "input_tokens": 0,
  "cached_input_tokens": 0,
  "cache_write_input_tokens": 0,
  "output_tokens": 0,
  "reasoning_tokens": 0,
  "total_tokens": 0
}
```

The adapter does **not** persist the raw OpenAI response envelope, response headers, provider error bodies, API credentials, or arbitrary provider metadata in this field.

`input_tokens`, `output_tokens`, and `total_tokens` must be present as non-negative integers when OpenAI supplies a `usage` object. Cached/cache-write/reasoning detail counters default to zero when their documented detail object or field is absent.

A `completed` response from the fixed production endpoint `https://api.openai.com/v1/responses` is not accepted as a successful Task if usage is absent or malformed. This keeps successful permanent-provider work from silently losing accounting. Synthetic/custom HTTPS endpoints used by offline tests may omit usage.

Structured metadata follows the same durable completion path as the Task result and is stored in SQLite schema v3. `gaudere-control task ID` exposes it as `result_metadata_content_type` and `result_metadata` through the single-owner service boundary.

The durable provider call budget remains independent of token accounting: a provider permit is consumed before the external effect boundary. Missing or malformed usage never refunds that permit and never makes an ambiguous effect safe to replay.

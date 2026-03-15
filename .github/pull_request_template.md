## Summary
- What changed and why.

## Validation
- Build/test commands run:
  - [ ] `make build SOC_BUILD=star6e`
  - [ ] `make build SOC_BUILD=maruko` (if applicable)
- Runtime smoke checks performed:
  - [ ] Star6E
  - [ ] Maruko (if applicable)

## Checklist
- [ ] `VERSION` updated for this PR.
- [ ] `HISTORY.md` updated with this PR's user-visible changes.
- [ ] Documentation updated for changed behavior.

### HTTP API Contract (required when HTTP behavior changes)
- [ ] `documentation/HTTP_API_CONTRACT.md` updated in this PR.
- [ ] Endpoint/payload/status changes match the contract exactly.
- [ ] Contract version updated appropriately (breaking vs non-breaking).

### HTTP API Design Guardrails
- [ ] Endpoints are lean and focused on direct operational value.
- [ ] JSON payloads remain simple and descriptive.
- [ ] No unnecessary endpoint proliferation or generic/ambiguous fields were introduced.

## Notes
- Star6E-first policy for SigmaStar API-touching functionality:
  implement and validate on Star6E first, then add explicit Maruko follow-up items.

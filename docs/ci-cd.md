# CI/CD

## What runs automatically

- **`ci.yml`** on every push and pull request:
  - `lint` — C compiled with `-Wall -Wextra -Werror`, Prettier over the web files.
  - `host-and-wasm` — build the host tools + wasm core, run the smoke test.
  - `gba-stub` — build the GBA stub (devkitARM) with `-Werror` + a header sanity check.
- **`deploy-web.yml`** on push to `main` that touches `web/`, `tools/`, or the
  `Makefile`: rebuilds the wasm core, syncs `web/` to S3, invalidates CloudFront.

## Local dev

```sh
make hooks     # one-time: enable the pre-commit hook (lint + smoke test)
make lint      # C -Werror + Prettier
```

Prettier autofix: `npx prettier --write web/index.html web/app.js`.

When `src/main.c` or the art changes, rebuild and commit the deployed binaries:

```sh
make websync && git add web/assets/stub.gba web/assets/title.bin web/assets/art.bin
```

## One-time setup (needs repo admin / AWS)

### 1. Branch protection — PRs only into `main`, owner may bypass

Requires all three CI jobs to pass and a pull request; `enforce_admins=false`
lets you (the owner) still push directly.

```sh
gh api -X PUT repos/ddurks/bookboy/branches/main/protection --input - <<'JSON'
{
  "required_status_checks": {
    "strict": true,
    "checks": [
      { "context": "lint (C warnings + Prettier)" },
      { "context": "host tools + wasm core" },
      { "context": "GBA code stub" }
    ]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": { "required_approving_review_count": 0 },
  "restrictions": null
}
JSON
```

### 2. AWS OIDC role for the deploy

The deploy assumes an IAM role via GitHub OIDC — no stored keys. Create the role
once, then set the repo variable:

```sh
gh variable set AWS_DEPLOY_ROLE_ARN --body 'arn:aws:iam::<ACCOUNT_ID>:role/bookboy-deploy'
```

**Trust policy** (lets only this repo assume the role). Note: this repo emits
GitHub's **immutable** OIDC subject — `repo:<login>@<user_id>/<repo>@<repo_id>:…`
— so the `sub` must match that form, not `repo:ddurks/bookboy:…`. Confirm the
live value by printing the token's `sub` claim in a throwaway workflow step.

```json
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": { "Federated": "arn:aws:iam::593615615124:oidc-provider/token.actions.githubusercontent.com" },
    "Action": "sts:AssumeRoleWithWebIdentity",
    "Condition": {
      "StringEquals": { "token.actions.githubusercontent.com:aud": "sts.amazonaws.com" },
      "StringLike": { "token.actions.githubusercontent.com:sub": [
        "repo:ddurks@13110312/bookboy@1310608434:*",
        "repo:ddurks/bookboy:*"
      ] }
    }
  }]
}
```

(If the account has no GitHub OIDC provider yet, add one for
`https://token.actions.githubusercontent.com`, audience `sts.amazonaws.com`.)

**Permissions policy** (least privilege for the deploy):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": ["s3:ListBucket"], "Resource": "arn:aws:s3:::bookboy.drawvid.com" },
    { "Effect": "Allow", "Action": ["s3:PutObject", "s3:DeleteObject"], "Resource": "arn:aws:s3:::bookboy.drawvid.com/*" },
    { "Effect": "Allow", "Action": ["cloudfront:CreateInvalidation"], "Resource": "arn:aws:cloudfront::<ACCOUNT_ID>:distribution/E16NJ1IMVM1Y6P" }
  ]
}
```

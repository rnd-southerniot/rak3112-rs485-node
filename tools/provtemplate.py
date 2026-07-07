#!/usr/bin/env python3
"""provtemplate.py — shared loader/resolver for tools/provision_template.json.

Both provision_node.py (CRM → ChirpStack) and provision_nvs.py (device NVS) load the
provisioning template through this module, so the *source of every value* lives in one
declarative place. Secrets stay as ${ENV:...} references resolved at run time from
firmware/.env / ~/.config/siot/... — nothing secret lives in the template or in git.

Token forms understood by the interpolator:
  ${ENV:KEY}            -> env[KEY]            (firmware/.env or process env)
  ${resolved.KEY}       -> ctx["resolved"][KEY] (runtime values, e.g. hardware_id)
  ${KEY}                -> ctx[KEY]            (identity: deveui/joineui/appkey/serial)

Contract: docs/PROVISIONING_API_CONTRACT.md
"""
import json
import os
import re

_TOKEN = re.compile(r"\$\{([^}]+)\}")
_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TEMPLATE = os.path.join(_HERE, "provision_template.json")
DEFAULT_FW_ENV = os.path.join(_HERE, "..", "firmware", ".env")

# Per-product provisioning templates (careflow = Modbus/planeB modbus; senseflow = I2C/planeB profile).
# The CRM-registration workflow (planeA) is shared; only the product identity + planeB device config
# differ. Default is careflow, so existing single-product callers are unchanged.
TEMPLATES = {
    "careflow": DEFAULT_TEMPLATE,
    "senseflow": os.path.join(_HERE, "provision_template_senseflow.json"),
}


def template_for_product(product="careflow"):
    """Absolute path to a product's provisioning template. Raises KeyError on an unknown product."""
    try:
        return TEMPLATES[product]
    except KeyError:
        raise KeyError(f"unknown product '{product}' — known: {sorted(TEMPLATES)}")


def load_template(path=DEFAULT_TEMPLATE):
    with open(path) as f:
        return json.load(f)


def read_env_file(path):
    """Parse a KEY=VALUE .env file (ignores blank/# lines). No inline-comment stripping —
    values are taken verbatim, matching the original tools' behaviour."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def build_env(fw_env_path=DEFAULT_FW_ENV):
    """Merge process env (CRM_* etc.) with firmware/.env (LORAWAN_*/MODBUS_*).
    firmware/.env wins on conflict."""
    return {**os.environ, **read_env_file(fw_env_path)}


def _lookup(expr, ctx, env):
    if expr.startswith("ENV:"):
        key = expr[4:]
        if key not in env:
            raise KeyError(f"environment variable {key} is not set "
                           "(source it from firmware/.env or ~/.config/siot/...)")
        return env[key]
    if expr.startswith("resolved."):
        return ctx.get("resolved", {})[expr[len("resolved."):]]
    if expr in ctx:
        return ctx[expr]
    raise KeyError(f"cannot resolve ${{{expr}}}")


def interpolate(obj, ctx, env):
    """Recursively replace ${...} tokens in a JSON-ish structure. Keys starting with '_'
    are dropped (documentation). A string that is exactly one token preserves the value's
    native type (so ${resolved.hardware_id} can stay an int)."""
    if isinstance(obj, str):
        m = _TOKEN.fullmatch(obj)
        if m:
            return _lookup(m.group(1), ctx, env)
        return _TOKEN.sub(lambda mm: str(_lookup(mm.group(1), ctx, env)), obj)
    if isinstance(obj, dict):
        return {k: interpolate(v, ctx, env) for k, v in obj.items() if not k.startswith("_")}
    if isinstance(obj, list):
        return [interpolate(v, ctx, env) for v in obj]
    return obj


def resolve_field(spec, env):
    """Resolve a single value spec (e.g. a planeB modbus field). Returns the env value when
    present and non-empty, else the spec's 'default', else the interpolated literal."""
    expr = spec["value"]
    m = _TOKEN.fullmatch(expr) if isinstance(expr, str) else None
    if m and m.group(1).startswith("ENV:"):
        key = m.group(1)[4:]
        if env.get(key):
            return env[key]
        return spec.get("default")
    return interpolate(expr, {}, env)


def _transform(val, name):
    if name == "lowercase":
        return val.lower()
    if name == "uppercase":
        return val.upper()
    return val


def resolve_identity(template, env):
    """Resolve deveui/joineui/appkey/serial from the template's valueSources against env.
    appkey is included — the caller is responsible for keeping it out of logs."""
    vs = template["valueSources"]
    out = {}
    for key in ("deveui", "joineui", "appkey"):
        spec = vs[key]
        raw = resolve_field(spec, env)
        out[key] = _transform(raw or "", spec.get("transform", ""))
    # derived serial: "<prefix>${deveui[-6:]|upper}". The slice/transform token isn't handled by the
    # generic interpolator, so take the literal prefix (everything before "${") from the template and
    # append the last-6 DevEUI nibbles. Prefix is per-product (careflow RAK3112-RS485-, senseflow
    # SENSEFLOW-EINK-), so read it from the template rather than hardcoding.
    prefix = vs["serial"]["value"].split("${", 1)[0]
    out["serial"] = prefix + out["deveui"][-6:].upper()
    return out


def validate_identity(idv):
    if not (len(idv["deveui"]) == 16 and len(idv["joineui"]) == 16 and len(idv["appkey"]) == 32):
        raise ValueError("LORAWAN_DEVEUI/JOINEUI must be 16 hex chars and APPKEY 32 — "
                         "check firmware/.env")


def strip_keys(d, *drop):
    """Shallow copy of d without the named keys or any '_'-prefixed (documentation) key.
    Used to turn an annotated template object into a clean API request body."""
    dropset = set(drop)
    return {k: v for k, v in d.items() if k not in dropset and not k.startswith("_")}

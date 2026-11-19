//! Transactional "set as default" (AGENTS.md rule 10).
//!
//! A switch is stop → move the port → promote in services.json → start and prove → cascade
//! re-render. Every failure unwinds to exactly the state the call found: the old default
//! running on the canonical port, the new one down, both files unchanged.
//!
//! The canonical number itself never changes (3306 stays 3306). What moves is *which*
//! instance holds it, which is why the cascade only has to re-render references that name
//! the two port keys involved.

use std::time::{Duration, Instant};

use crate::engine::EngineState;
use crate::instance::RunState;
use crate::kinds;
use crate::log;

const READY_TIMEOUT: Duration = Duration::from_secs(15);
const POLL: Duration = Duration::from_millis(100);

pub(crate) fn promote_default(state: &mut EngineState, id: &str) -> Result<(), String> {
    let Some(target) = state.registry.find(id).cloned() else {
        return fail(format!("{id} 不是已安装的实例"));
    };
    let kind = target.kind.clone();
    let Some(meta) = kinds::find(&kind) else {
        return fail(format!("{id} 属于未知类型 \"{kind}\""));
    };
    let previous_id = state.registry.default_id(&kind).map(str::to_string);
    if previous_id.as_deref() == Some(id) {
        return Ok(()); // already the default: nothing to move
    }
    let previous = previous_id
        .as_deref()
        .and_then(|previous| state.registry.find(previous))
        .cloned();

    // php/python have no canonical port, so there is no number to move; only the renderers'
    // idea of "which instance the kind points at" changes.
    let owns_port = !meta.canonical.is_empty() && !target.port_key.is_empty();
    let canonical = if owns_port {
        match state.ports.canonical_port(meta.canonical) {
            Some(port) if port > 0 => port,
            _ => {
                return fail(format!(
                    "ports.json 里没有 \"{}\" 的规范端口",
                    meta.canonical
                ))
            }
        }
    } else {
        0
    };
    let target_was = state.ports.port_of(&target.port_key);
    let target_was_role = state.ports.role_of(&target.port_key).to_string();
    let previous_holds_canonical = previous
        .as_ref()
        .map(|item| {
            owns_port
                && state.ports.port_of(&item.port_key) == canonical
                && state.ports.role_of(&item.port_key) == "default"
        })
        .unwrap_or(false);

    // ------------------------------------------------------------- 1. take them down
    let mut stopped: Vec<String> = Vec::new();
    let bring_back = |state: &mut EngineState, stopped: &[String]| {
        for which in stopped.iter().rev() {
            if let Err(error) = state.start(which) {
                // This service was up before the call and is down now. Loud, because the
                // only way out is for a human to start it.
                log::error(&format!(
                    "switch: CRITICAL, {which} could not be brought back: {error}"
                ));
            }
        }
    };

    if let Some(previous_id) = previous_id.clone() {
        if state.is_running(&previous_id) {
            if let Err(error) = state.stop(&previous_id) {
                return fail(format!(
                    "{previous_id} 停不下来（{error}），没有开始切换"
                ));
            }
            stopped.push(previous_id);
        }
    }
    if state.is_running(id) {
        if let Err(error) = state.stop(id) {
            bring_back(state, &stopped);
            return fail(format!("{id} 停不下来（{error}），切换已中止"));
        }
        stopped.push(id.to_string());
    }

    // ---------------------------------------------------------- 2. move the port
    let mut ports_moved = false;
    let used_swap = previous_holds_canonical;
    if owns_port {
        ports_moved = if used_swap {
            let previous_key = previous
                .as_ref()
                .map(|item| item.port_key.clone())
                .unwrap_or_default();
            state.ports.swap_roles(&previous_key, &target.port_key)
        } else {
            state
                .ports
                .assign_canonical(meta.canonical, &target.port_key, &kind)
        };
        if !ports_moved {
            bring_back(state, &stopped);
            return fail(format!("{id} 无法占用端口 {canonical}，已回滚"));
        }
    }
    let undo_ports = |state: &mut EngineState| {
        if !ports_moved {
            return;
        }
        let ok = if used_swap {
            // swap_roles is its own inverse: the target now holds the default role, so
            // passing it first puts both ports and roles back.
            let previous_key = previous
                .as_ref()
                .map(|item| item.port_key.clone())
                .unwrap_or_default();
            state.ports.swap_roles(&target.port_key, &previous_key)
        } else {
            let mut ok = state.ports.release(&target.port_key);
            if ok && target_was > 0 {
                ok = state
                    .ports
                    .rebind(&target.port_key, target_was, &target_was_role, &kind);
            }
            ok
        };
        if !ok {
            log::error(&format!(
                "switch: CRITICAL, ports.json was not restored for {id}"
            ));
        }
    };

    // ------------------------------------------------------------ 3. flip the role
    if !state.registry.promote_default(id) {
        undo_ports(state);
        bring_back(state, &stopped);
        return fail(format!(
            "{id} 无法写为 {kind} 的默认实例，已回滚"
        ));
    }
    // Both cached drivers were built against the old binding and the old port.
    let _ = state.invalidate(id);
    if let Some(previous_id) = previous_id.as_deref() {
        let _ = state.invalidate(previous_id);
    }

    // ---------------------------------------------------- 4. start and prove the new one
    // Only when this kind was actually serving: switching the default of a stopped stack
    // must not start it as a side effect.
    let was_serving = !stopped.is_empty();
    let mut failure: Option<String> = None;
    if was_serving {
        if let Err(error) = state.start(id) {
            failure = Some(format!("{id} 没有启动：{error}"));
        } else if !wait_ready(state, id, READY_TIMEOUT) {
            let detail = state
                .driver(id)
                .map(|driver| driver.status())
                .map(|status| status.detail)
                .unwrap_or_default();
            failure = Some(format!(
                "{id} 在端口 {} 上没有响应（{detail}）",
                if owns_port { canonical } else { 0 }
            ));
        }
    }

    if let Some(reason) = failure {
        // Back to the state this call found: new default down, role restored, ports
        // restored, drivers rebuilt, whatever was running up again.
        let _ = state.stop(id);
        if let Some(previous_id) = previous_id.as_deref() {
            if !state.registry.promote_default(previous_id) {
                log::error(&format!(
                    "switch: CRITICAL, services.json still calls {id} the default of {kind}"
                ));
            }
        }
        undo_ports(state);
        let _ = state.invalidate(id);
        if let Some(previous_id) = previous_id.as_deref() {
            let _ = state.invalidate(previous_id);
        }
        bring_back(state, &stopped);
        return fail(format!("{reason}；已回滚"));
    }

    // ----------------------------------------------------------------- 5. re-render
    // Nginx vhosts are the only place a service port is referenced. Re-rendering after the
    // switch is the cascade; a failure here rolls the whole transaction back.
    if let Err(error) = state.rerender_nginx() {
        let _ = state.stop(id);
        if let Some(previous_id) = previous_id.as_deref() {
            let _ = state.registry.promote_default(previous_id);
        }
        undo_ports(state);
        let _ = state.invalidate(id);
        if let Some(previous_id) = previous_id.as_deref() {
            let _ = state.invalidate(previous_id);
        }
        bring_back(state, &stopped);
        return fail(format!("重新渲染 nginx 失败（{error}），已回滚"));
    }

    log::info(&format!(
        "switch: default of {kind} is now {id} on port {}",
        if owns_port { canonical } else { 0 }
    ));
    Ok(())
}

fn wait_ready(state: &mut EngineState, id: &str, timeout: Duration) -> bool {
    let started = Instant::now();
    while started.elapsed() < timeout {
        let status = match state.driver(id) {
            Ok(driver) => driver.status(),
            Err(_) => return false,
        };
        match status.state {
            RunState::Running => return true,
            // It is not going to become ready.
            RunState::Crashed | RunState::Stopped => return false,
            _ => {}
        }
        std::thread::sleep(POLL);
    }
    false
}

fn fail(message: String) -> Result<(), String> {
    log::error(&format!("switch: {message}"));
    Err(message)
}

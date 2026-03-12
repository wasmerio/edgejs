use std::cell::Cell;
use std::sync::Mutex;

use wasmer::{FunctionEnvMut, StoreMut, Table, Value};

use crate::{RuntimeEnv, UNOFFICIAL_ENV_HANDLE};

// Thread-local raw pointer to the current FunctionEnvMut, used by the
// C++ → Rust callback trampoline. Set before any C++ FFI call that might
// trigger V8 callbacks, cleared after. Safe because:
// 1. Single-threaded WASM execution
// 2. Pointer is valid for the duration of the synchronous FFI call
// 3. Callback is strictly nested within the FFI call
thread_local! {
    pub static CB_ENV_PTR: Cell<*mut ()> = Cell::new(std::ptr::null_mut());
}

static CB_TOP_LEVEL: Mutex<Option<CallbackTopLevelState>> = Mutex::new(None);

#[derive(Clone)]
struct CallbackTopLevelState {
    store: *mut StoreMut<'static>,
    table: Table,
}

unsafe impl Send for CallbackTopLevelState {}

fn call_guest_callback(
    store: &mut impl wasmer::AsStoreMut,
    table: &Table,
    wasm_fn_ptr: u32,
    data_val: u64,
) -> u32 {
    let Some(elem) = table.get(store, wasm_fn_ptr) else {
        return 0;
    };
    let func = match elem {
        Value::FuncRef(Some(func)) => func,
        Value::FuncRef(None) => return 0,
        _ => return 0,
    };
    match func.call(
        store,
        &[
            Value::I32(UNOFFICIAL_ENV_HANDLE),
            Value::I32(data_val as i32),
        ],
    ) {
        Ok(ret_vals) => match ret_vals.first() {
            Some(Value::I32(v)) => *v as u32,
            Some(Value::I64(v)) => *v as u32,
            _ => 0,
        },
        Err(err) => {
            eprintln!("[callback trampoline] error calling function: {err}");
            0
        }
    }
}

pub fn set_top_level_callback_state(store: &mut StoreMut<'_>, table: Option<Table>) {
    let mut guard = CB_TOP_LEVEL
        .lock()
        .expect("callback top-level mutex poisoned");
    if let Some(table) = table {
        let raw: *mut StoreMut<'static> =
            unsafe { std::mem::transmute(store as *mut StoreMut<'_>) };
        *guard = Some(CallbackTopLevelState { store: raw, table });
    } else {
        *guard = None;
    }
}

/// Rust trampoline called from C++ when a V8 callback fires.
/// Retrieves the WASM store from the thread-local, then calls
/// __napi_callback_dispatch in the WASM guest.
#[no_mangle]
pub extern "C" fn snapi_host_invoke_wasm_callback(wasm_fn_ptr: u32, data_val: u64) -> u32 {
    CB_ENV_PTR.with(|cell| {
        let ptr = cell.get();
        if !ptr.is_null() {
            // SAFETY: ptr was set from &mut FunctionEnvMut<RuntimeEnv> which is still
            // alive on the call stack above us. Single-threaded, synchronous.
            let env: &mut FunctionEnvMut<'_, RuntimeEnv> =
                unsafe { &mut *(ptr as *mut FunctionEnvMut<'_, RuntimeEnv>) };
            let Some(table) = env.data().table.clone() else {
                return 0;
            };
            let (_, mut store) = env.data_and_store_mut();
            return call_guest_callback(&mut store, &table, wasm_fn_ptr, data_val);
        }

        let state = CB_TOP_LEVEL
            .lock()
            .expect("callback top-level mutex poisoned")
            .clone();
        if let Some(state) = state {
            let store = unsafe { &mut *state.store };
            return call_guest_callback(store, &state.table, wasm_fn_ptr, data_val);
        }
        eprintln!("[callback trampoline] no env pointer available");
        0
    })
}

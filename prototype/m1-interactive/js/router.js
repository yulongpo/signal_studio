export function createRouter(state, render) {
  return {
    go(view) {
      state.view = view;
      render();
    },
    openWorkbench() {
      state.view = 'workbench';
      render();
    },
    openTasks() {
      state.view = 'tasks';
      render();
    },
    openWelcome() {
      state.view = 'welcome';
      render();
    }
  };
}

from pyautogui import press, typewrite, hotkey
from time import sleep
import pygetwindow as gw

def list_and_focus_window(target_window_name):
    # Get all windows
    all_windows = gw.getAllWindows()

    print("Active Windows:")
    for window in all_windows:
        if window.title:  # Only print windows with non-empty titles
            print(f"- {window.title}")

    # Find and focus the target window                                                      
    target_windows = gw.getWindowsWithTitle(target_window_name)
    
    if target_windows:
        target_window = target_windows[0]  # Get the first matching window
        try:
            target_window.activate()
            print(f"\nBrought '{target_window.title}' to the foreground.")
        except Exception as e:
            print(f"\nError focusing window: {e}")
    else:
        print(f"\nNo window found with the name '{target_window_name}'.")

sleep(19)
list_and_focus_window("Google Chrome")
sleep(3)
press('tab')
press('tab')
press('enter')
sleep(9)
press('tab')
press('tab')
press('enter')
sleep(9)    
press('tab')
press('tab')
press('tab')
#press('enter')
press('tab')
press('tab')            
press('tab')
press('enter')
'''sleep(9)
press('tab')
press('tab')
press('tab')
press('tab')
press('space')
press('tab')
press('tab')
press('space')
press('tab')
press('tab')
press('space')
press('tab')
press('tab')
press('space')
press('tab')
press('tab')
press('tab')
press('tab')
press('tab')
press('space')'''
sleep(5)
hotkey('ctrl', 'w')

exit()

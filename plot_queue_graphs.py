import pandas as pd
import matplotlib.pyplot as plt
import glob
import sys
import os

# 1. Automatically find the most recently generated queue CSV file
queue_files = glob.glob('queue_series_*.csv')

if not queue_files:
    print("Error: No 'queue_series_*.csv' files found. Run your ns-3 simulation first.")
    sys.exit()

# Sort by modification time to get the latest one
latest_csv = max(queue_files, key=os.path.getmtime)
print(f"Reading queue data from: {latest_csv}")

df = pd.read_csv(latest_csv)

# Extract algorithm name from filename for the title
algo_name = latest_csv.replace('queue_series_', '').replace('.csv', '')

# 2. Set up the figure
fig, ax = plt.subplots(figsize=(10, 6))

# 3. Plot the buffer occupancy for each server's queue
# Server 1 (Legacy) has a strict 20p limit, while Servers 2 & 3 have 1000p
ax.plot(df['Time_Seconds'], df['Server_1_Queue'], 
        label='Server 1 Queue (Legacy, Max: 20p)', color='#E63946', linewidth=2.5)
        
ax.plot(df['Time_Seconds'], df['Server_2_Queue'], 
        label='Server 2 Queue (Modern, Max: 1000p)', color='#F4A261', linewidth=2, linestyle='--')
        
ax.plot(df['Time_Seconds'], df['Server_3_Queue'], 
        label='Server 3 Queue (Modern, Max: 1000p)', color='#2A9D8F', linewidth=2.5)

# 4. Add failure annotation at t = 4.0s
ax.axvline(x=4.0, color='black', linestyle=':', linewidth=2)
ax.annotate('Server 2 Hard Failure\n(t = 4.0s)', 
            xy=(4.0, 50), 
            xytext=(4.5, 100),
            arrowprops=dict(facecolor='black', shrink=0.05, width=1.5, headwidth=8), 
            fontsize=11, fontweight='bold')

# 5. Format the chart
ax.set_title(f'Queue Buffer Occupancy Over Time [{algo_name}]', fontsize=14, fontweight='bold')
ax.set_xlabel('Simulation Time (seconds)', fontsize=12)
ax.set_ylabel('Packets in Queue (Buffer Occupancy)', fontsize=12)
ax.legend(loc='upper right', fontsize=11)
ax.grid(True, linestyle='--', alpha=0.6)

# 6. Save and display
output_filename = f'queue_occupancy_{algo_name}.png'
plt.tight_layout()
plt.savefig(output_filename, dpi=300)
print(f"Queue graph saved successfully as '{output_filename}'")
plt.show()

import pandas as pd
import matplotlib.pyplot as plt
import glob
import os
import sys

# 1. Find the most recently generated time-series CSV file
csv_files = glob.glob('time_series_*.csv')

if not csv_files:
    print("Error: No 'time_series_*.csv' files found.")
    sys.exit()

latest_csv = max(csv_files, key=os.path.getmtime)
print(f"Reading traffic data from: {latest_csv}")

df = pd.read_csv(latest_csv)

# 2. Calculate the total cumulative traffic across the entire data center
df['Total_Client_Requests'] = df['Server_1_Requests'] + df['Server_2_Requests'] + df['Server_3_Requests']

# 3. Set up the figure
fig, ax = plt.subplots(figsize=(10, 6))

ax.plot(df['Time_Seconds'], df['Total_Client_Requests'], 
        label='Total Network Load (Cumulative)', color='#1D3557', linewidth=3)

# 4. Highlight the traffic generation window (2.0s to 14.0s)
ax.axvspan(2.0, 14.0, color='#457B9D', alpha=0.1, label='Active Traffic Generation Window')
ax.axvline(x=2.0, color='gray', linestyle=':', linewidth=2)
ax.axvline(x=14.0, color='gray', linestyle=':', linewidth=2)

# 5. Format the chart
ax.set_title('Total Client Traffic Generation (15-Second Simulation)', fontsize=14, fontweight='bold')
ax.set_xlabel('Simulation Time (seconds)', fontsize=12)
ax.set_ylabel('Total Cumulative Requests', fontsize=12)
ax.set_xlim(0, 15)
ax.legend(loc='upper left', fontsize=11)
ax.grid(True, linestyle='--', alpha=0.6)

# 6. Save and display
output_filename = 'new_traffic_pattern.png'
plt.tight_layout()
plt.savefig(output_filename, dpi=300)
print(f"Traffic graph saved successfully as '{output_filename}'")
plt.show()

# Smart Rail Load Balancing System (Group 3)

|Catarina Ferreira|Daniel Alvo|Francisca Almeida|
|-------|-------|-------|
ist1116658 |ist1102914|ist1105901|

## Introduction

Urban rail systems frequently suffer from uneven passenger distribution across trains cars. During peak house, some carriages become overcrowded while others remain underutilized.

This imbalance leads to:
- Passenger discomfort and safety risks.
- Longer boarding and alighting time.
- Increased dwell time at stations.
- Poor passenger experience.
- Reduced overall system efficiency.
- Uneven wear on train cars.

Currently, passenger choose train cars without real-time information about carriage occupancy. Decision are based on habits, proximity to stairs, or guesswork rather than data.

## Literature Review

[Estimating congestion in train cars
by using BLE Signals](https://yukimat.jp/data/pdf/paper/BLECE-Train_c_202205_taya_DICPS.pdf)

[A Review of Passenger Counting in Public Transport Concepts with Solution Proposal Based on Image Processing and Machine Learning](https://www.mdpi.com/2673-4117/5/4/172) 

## Problem

There is **no real-time, intuitive feedback system** that guides passengers to distribute themselves evenly across train cars before boarding.

As a result, rail systems operate below optimal efficiency despite having available capacity.

### Solution Requirements

**Functional Requirements**

FR-1: Occupancy Detection  
- The system shall measure the occupancy level of each carriage in real time.

FR-2: Data Processing  
- The system shall convert raw (weight) data into an occupancy percentage (0–100%).

FR-3: Status Classification  
- The system shall classify occupancy into predetermined states.

FR-4: Visual Feedback
- The system shall display the occupancy state externally.

FR-5: Real-Time Updates  
- The system shall update occupancy indicators with an adequate frequency.

FR-6: Multi Carriage Integration
- The system shall operate independently for each carriage but allow centralized monitoring (optional advanced feature).

FR-7: External Management
- 

**Quality Attributes**

QA-1: Reliability  
- The system shall operate continuously during train operation hours.

QA-2: Accuracy  
- Occupancy estimation error shall not exceed a certain percentage.

QA-3: Response Time
- System response time must be < 5 seconds.

QA-4: Usability  
- The color system shall be intuitive and understandable without training.

QA-5: Visibility  
- LED indicators shall be visible in:
    - Daylight
    - Night
    - Rain conditions

QA-6: Scalability  


**Ambient Intelligence Requirements**

AI-1: Context Awareness  
- The system should limit ambiguity.
AI-2: Non-Intrusiveness
- The system shall not require passenger interaction.
AI-3: Adaptivity  
- The system shall dynamically update occupancy state without manual intervention.
AI-4: Human-Centered Design
- Information shall reduce stress and improve passenger comfort.

### Assumptions
Francisca

## Prototype

### Overview
Francisca

### Logical design

### Technology selection

## Bill-of-materials

### Hardware
All

### Software
All

## Plan
Francisca

## Bibliography

Taya, E., Kanamitsu, Y., Tachibana, K., Nakamura, Y., Matsuda, Y., Suwa, H., & Yasumoto, K. (2022). *Estimating congestion in train cars by using BLE signals*. In 2022 IEEE International Conference on Distributed Computing Systems Workshops (ICDCSW). [https://yukimat.jp/data/pdf/paper/BLECE-Train_c_202205_taya_DICPS.pdf](https://yukimat.jp/data/pdf/paper/BLECE-Train_c_202205_taya_DICPS.pdf)

Radovan, A., Mršić, L., Đambić, G., & Mihaljević, B. (2024). A review of passenger counting in public transport concepts with solution proposal based on image processing and machine learning. *Eng, 5*(4), 3284–3315. [https://doi.org/10.3390/eng5040172](https://doi.org/10.3390/eng5040172)

## Declaration of Use of AI
Francisca

